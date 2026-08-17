#include "xpcog/core/audio/AudioEngine.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace xpcog {
namespace {

/// How long the feeder sleeps when the ring is full. Short relative to the ring,
/// so it wakes well before the device runs dry.
constexpr auto kFeederBackoff = std::chrono::milliseconds(2);

}  // namespace

struct AudioEngine::OpenTrack {
    SourcePtr  source;
    DecoderPtr decoder;
    Url        url;
};

AudioEngine::AudioEngine(const PluginRegistry& registry, IAudioOutput& output,
                         RingBuffer& ring, const Settings& settings)
    : registry_(registry), output_(output), settings_(settings), ring_(ring) {
    // Order is the signal path. The equaliser sits after conversion, so it
    // always runs at the device's rate and its coefficients survive a track
    // whose source rate differs.
    chain_.push_back(&equalizer_);
}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::openTrack(const Url& url) {
    auto opened = registry_.open(url);
    if (!opened) {
        return false;
    }

    auto track     = std::make_unique<OpenTrack>();
    track->source  = std::move(opened.source);
    track->decoder = std::move(opened.decoder);
    track->url     = url;
    track_         = std::move(track);
    return true;
}

void AudioEngine::closeTrack() {
    if (track_) {
        // Decoder first: it borrows the source and must not outlive it.
        track_->decoder.reset();
        track_->source.reset();
        track_.reset();
    }
}

bool AudioEngine::play(const Url& url) {
    stop();

    if (!openTrack(url)) {
        return false;
    }

    const TrackProperties firstProps = track_->decoder->properties();
    format_ = firstProps.format;
    if (!format_.valid()) {
        closeTrack();
        return false;
    }

    // The device runs at the first track's rate and channel count and then stays
    // there; later tracks are resampled to match. Reconfiguring the device
    // mid-stream is what would otherwise make a format change audible as a gap.
    format_.format        = SampleFormat::F32;
    format_.bitsPerSample = 32;

    if (!converter_.setOutputFormat(format_.sampleRate, format_.channels,
                                    settings_.Resampling())) {
        closeTrack();
        return false;
    }
    converter_.reset();
    converter_.setHdcdEnabled(settings_.EnableHDCD());
    applyReplayGain(firstProps);

    // The chain runs at the device format, so it is sized here rather than per
    // track: a track at another sample rate is resampled to this one before it
    // reaches the equaliser, which is what lets the filter state survive a
    // format-changing seam instead of being rebuilt mid-album.
    for (DSPNode* node : chain_) {
        node->prepare(format_);
        node->reset();
    }
    applyDspSettings();
    dspDirty_.store(false, std::memory_order_relaxed);

    // The ring is the caller's and already sized; just make sure nothing is left
    // over from a previous track.
    ring_.clear();

    framesWritten_ = 0;
    {
        std::lock_guard lock(seamMutex_);
        pendingSeams_.clear();
        seekPlayedBase_ = 0;
        seekTrackBase_  = 0;
        audibleUrl_        = url;
        audibleTrackStart_ = 0;
    }
    {
        std::lock_guard lock(finishedMutex_);
        finished_ = false;
    }

    IAudioOutput::Config config;
    config.sampleRate = format_.sampleRate;
    config.channels   = format_.channels;

    // Fill the ring before the device starts. Otherwise the first callbacks land
    // before the feeder has produced anything and are counted as underruns --
    // real ones, audible as a click at the start of playback.
    {
        AudioChunk        chunk;
        const std::size_t target = ring_.capacity() / 2;
        while (ring_.availableToRead() < target &&
               track_->decoder->readAudio(chunk)) {
            converted_.clear();
            if (!converter_.process(chunk, converted_) || converted_.empty()) {
                continue;
            }
            applyDsp();
            ring_.write(converted_.data(), converted_.size());
            framesWritten_ += converted_.size() / format_.channels;
        }
    }

    if (!output_.start(config)) {
        closeTrack();
        return false;
    }

    running_.store(true, std::memory_order_release);
    status_.store(PlaybackStatus::Playing, std::memory_order_relaxed);
    feeder_ = std::thread([this] { feederLoop(); });

    if (delegate_ != nullptr) {
        delegate_->trackBegan(url);
    }
    return true;
}

void AudioEngine::applyReplayGain(const TrackProperties& props) {
    converter_.setGain(replayGainScale(props.replayGain, settings_.VolumeScaling()));
}

void AudioEngine::applyDspSettings() {
    equalizer_.setPreamp(settings_.EqPreamp());

    const auto keys = Equalizer::bandSettingsKeys();
    std::array<double, Equalizer::kBands> gains{};
    for (std::size_t band = 0; band < keys.size(); ++band) {
        // By key rather than by generated accessor: naming 31 of them here would
        // be 31 chances to pair a band with the wrong frequency, and the table
        // that pairs them already exists next to the frequencies.
        const std::string raw = settings_.rawValue(keys[band]);
        gains[band]           = raw.empty() ? 0.0 : std::strtod(raw.c_str(), nullptr);
    }
    equalizer_.setBandGains(gains);
}

void AudioEngine::applyDsp() {
    if (converted_.empty()) {
        return;
    }
    const std::size_t frames = converted_.size() / format_.channels;
    for (DSPNode* node : chain_) {
        if (node->active()) {
            node->process(converted_.data(), frames);
        }
    }
}

bool AudioEngine::writeSamples(const float* samples, std::size_t count) {
    std::size_t written = 0;
    while (written < count) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        written += ring_.write(samples + written, count - written);
        if (written < count) {
            std::this_thread::sleep_for(kFeederBackoff);
        }
    }
    return true;
}

bool AudioEngine::writeToRing(const AudioChunk& chunk) {
    if (chunk.frameCount() == 0) {
        return true;
    }

    converted_.clear();
    if (!converter_.process(chunk, converted_)) {
        // A layout with no float conversion (raw DSD, M6). Skip it rather than
        // emitting noise at the wrong scale.
        return true;
    }
    if (converted_.empty()) {
        return true;  // the resampler is still filling its delay line
    }

    applyDsp();

    if (!writeSamples(converted_.data(), converted_.size())) {
        return false;
    }

    framesWritten_ += converted_.size() / format_.channels;
    return true;
}

void AudioEngine::feederLoop() {
    AudioChunk chunk;

    while (running_.load(std::memory_order_acquire)) {
        publishSeams();

        // Here rather than in reloadDsp(): this is the feeder thread, the only
        // one that may touch the coefficients while process() is not running.
        if (dspDirty_.exchange(false, std::memory_order_relaxed)) {
            applyDspSettings();
        }

        if (const std::int64_t requested =
                pendingSeek_.exchange(-1, std::memory_order_acq_rel);
            requested >= 0) {
            performSeek(requested);
        }

        // Nothing may be written until the consumer has dropped the pre-seek
        // audio, or the discard would take the post-seek audio with it. If the
        // device is not running -- paused, or stopped for a reconfigure -- this
        // simply waits, and the first callback after it resumes does the drop.
        if (ring_.flushPending()) {
            std::this_thread::sleep_for(kFeederBackoff);
            continue;
        }

        if (seekBasePending_) {
            // The stale audio is gone, so what the device reports having played
            // is now the true starting point for the new position.
            std::lock_guard lock(seamMutex_);
            seekPlayedBase_  = output_.framesPlayed();
            seekTrackBase_   = pendingSeekTrack_;
            framesWritten_   = seekPlayedBase_;
            seekBasePending_ = false;
        }

        if (!track_ || !track_->decoder->readAudio(chunk)) {
            // End of the current decoder. Ask for the next track *now*, while the
            // audio already in the ring is still playing out -- this is what makes
            // the handoff gapless. Cog does the same in -endOfInputReached:.
            std::optional<Url> next =
                (delegate_ != nullptr) ? delegate_->nextTrack() : std::nullopt;

            bool advanced = false;
            while (next.has_value()) {
                const Url candidate = *next;
                closeTrack();

                if (openTrack(candidate)) {
                    // A different sample rate or channel count no longer ends
                    // playback: the converter resamples the new track into the
                    // device format that is already running, so the handoff stays
                    // gapless. Only the ReplayGain scale has to be re-read.
                    //
                    // Flush the resampler first. Reconfiguring it for the new rate
                    // discards its delay line, which holds the last few
                    // milliseconds of the outgoing track -- exactly the samples
                    // that meet the seam.
                    converted_.clear();
                    converter_.drain(converted_);
                    applyDsp();
                    if (!converted_.empty() &&
                        !writeSamples(converted_.data(), converted_.size())) {
                        break;
                    }

                    applyReplayGain(track_->decoder->properties());

                    {
                        std::lock_guard lock(seamMutex_);
                        pendingSeams_.push_back(Seam{framesWritten_, candidate});
                    }
                    advanced = true;
                    break;
                }

                if (delegate_ != nullptr) {
                    delegate_->trackFailed(candidate);
                    next = delegate_->nextTrack();
                } else {
                    next.reset();
                }
            }

            if (!advanced) {
                break;
            }
            continue;
        }

        if (!writeToRing(chunk)) {
            break;
        }
    }

    // Flush the resampler's delay line, or the last fraction of a second of the
    // final track is silently dropped.
    if (running_.load(std::memory_order_acquire)) {
        converted_.clear();
        converter_.drain(converted_);
        applyDsp();
        if (!converted_.empty()) {
            writeSamples(converted_.data(), converted_.size());
        }
    }

    // Let the device play out what is still buffered before declaring the end.
    while (running_.load(std::memory_order_acquire) && ring_.availableToRead() > 0) {
        publishSeams();
        std::this_thread::sleep_for(kFeederBackoff);
    }
    publishSeams();

    const bool naturalEnd = running_.load(std::memory_order_acquire);
    if (naturalEnd) {
        status_.store(PlaybackStatus::Stopped, std::memory_order_relaxed);
        if (delegate_ != nullptr) {
            delegate_->stoppedNaturally();
        }
    }

    {
        std::lock_guard lock(finishedMutex_);
        finished_ = true;
    }
    finishedCv_.notify_all();
}

void AudioEngine::performSeek(std::int64_t frame) {
    if (!track_) {
        return;
    }

    const std::int64_t reached = track_->decoder->seek(frame);
    if (reached < 0) {
        return;  // the decoder declined; stay where we are
    }

    // The resampler and the HDCD decoder both carry state from the old position.
    // Keeping it would bleed a few milliseconds of the previous location into
    // the new one, which is audible as a click at exactly the moment a user is
    // listening for the jump to land.
    converter_.reset();
    converted_.clear();

    // Same reasoning for the chain: a biquad holding two samples from the old
    // position rings them into the new one, which is the click a user hears at
    // precisely the moment they are listening for the seek to land.
    for (DSPNode* node : chain_) {
        node->reset();
    }

    ring_.requestFlush();

    // The base cannot be taken here. Everything still in the ring is about to be
    // *discarded*, so those frames are never delivered and framesPlayed() will
    // never account for them -- adding them to the base put it up to a ring
    // ahead of reality, and with a three-second ring the clock reported the old
    // position for three seconds after every seek. It is taken below instead,
    // once the consumer has acknowledged the flush and framesPlayed() is
    // truthful again.
    pendingSeekTrack_ = static_cast<std::uint64_t>(reached);
    seekBasePending_  = true;
}

bool AudioEngine::seek(double seconds) {
    if (status_.load(std::memory_order_relaxed) == PlaybackStatus::Stopped) {
        return false;
    }
    const double rate = format_.sampleRate;
    if (rate <= 0.0) {
        return false;
    }

    const double clamped = (seconds > 0.0) ? seconds : 0.0;
    pendingSeek_.store(static_cast<std::int64_t>(clamped * rate),
                       std::memory_order_release);
    return true;
}

void AudioEngine::publishSeams() {
    const std::uint64_t played = output_.framesPlayed();

    for (;;) {
        Url became;
        {
            std::lock_guard lock(seamMutex_);
            if (pendingSeams_.empty() || pendingSeams_.front().framePosition > played) {
                return;
            }
            const Seam seam = pendingSeams_.front();
            pendingSeams_.pop_front();
            audibleUrl_        = seam.url;
            audibleTrackStart_ = seam.framePosition;
            became             = seam.url;
            // A new track starts at zero, so any seek base from the previous one
            // no longer applies.
            seekPlayedBase_ = 0;
            seekTrackBase_  = 0;
        }
        if (delegate_ != nullptr) {
            delegate_->trackBegan(became);
        }
    }
}

void AudioEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (feeder_.joinable()) {
        feeder_.join();
    }
    output_.stop();
    closeTrack();
    status_.store(PlaybackStatus::Stopped, std::memory_order_relaxed);

    {
        std::lock_guard lock(finishedMutex_);
        finished_ = true;
    }
    finishedCv_.notify_all();
}

void AudioEngine::pause() {
    if (status() == PlaybackStatus::Playing) {
        output_.pause();
        status_.store(PlaybackStatus::Paused, std::memory_order_relaxed);
    }
}

void AudioEngine::resume() {
    if (status() == PlaybackStatus::Paused) {
        output_.resume();
        status_.store(PlaybackStatus::Playing, std::memory_order_relaxed);
    }
}

void AudioEngine::waitUntilFinished() {
    std::unique_lock lock(finishedMutex_);
    finishedCv_.wait(lock, [this] { return finished_; });
}

void AudioEngine::setVolume(float gain) { output_.setVolume(gain); }
float AudioEngine::volume() const { return output_.volume(); }

double AudioEngine::playedSeconds() const {
    const double rate = format_.sampleRate;
    return (rate > 0.0) ? static_cast<double>(output_.framesPlayed()) / rate : 0.0;
}

double AudioEngine::trackPositionSeconds() const {
    const double rate = format_.sampleRate;
    if (rate <= 0.0) {
        return 0.0;
    }
    std::uint64_t start     = 0;
    std::uint64_t seekPlayed = 0;
    std::uint64_t seekTrack  = 0;
    {
        std::lock_guard lock(seamMutex_);
        start      = audibleTrackStart_;
        seekPlayed = seekPlayedBase_;
        seekTrack  = seekTrackBase_;
    }

    const std::uint64_t played = output_.framesPlayed();

    // After a seek the track no longer began where the device's frame counter
    // says it did, so the offset is measured from the seek instead. A later
    // track change moves audibleTrackStart_ past the seek base and takes over.
    if (seekPlayed > start && played >= seekPlayed) {
        return static_cast<double>(seekTrack + (played - seekPlayed)) / rate;
    }
    return (played > start) ? static_cast<double>(played - start) / rate : 0.0;
}

}  // namespace xpcog
