#include "xpcog/core/audio/AudioEngine.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <chrono>
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
                         RingBuffer& ring)
    : registry_(registry), output_(output), ring_(ring) {}

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

    format_ = track_->decoder->properties().format;
    if (!format_.valid()) {
        closeTrack();
        return false;
    }

    // The ring is the caller's and already sized; just make sure nothing is left
    // over from a previous track.
    ring_.clear();

    framesWritten_ = 0;
    {
        std::lock_guard lock(seamMutex_);
        pendingSeams_.clear();
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

bool AudioEngine::writeToRing(const AudioChunk& chunk) {
    const std::size_t samples = float32SampleCount(chunk);
    if (samples == 0) {
        return true;
    }

    static thread_local std::vector<float> scratch;
    scratch.resize(samples);
    if (convertToFloat32(chunk, scratch) != samples) {
        return true;  // unsupported layout (DSD); skip rather than emit noise
    }

    std::size_t written = 0;
    while (written < samples) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        written += ring_.write(scratch.data() + written, samples - written);
        if (written < samples) {
            std::this_thread::sleep_for(kFeederBackoff);
        }
    }

    framesWritten_ += chunk.frameCount();
    return true;
}

void AudioEngine::feederLoop() {
    AudioChunk chunk;

    while (running_.load(std::memory_order_acquire)) {
        publishSeams();

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
                    const AudioFormat nextFormat =
                        track_->decoder->properties().format;

                    // A format change would need the device reconfigured, which
                    // cannot be gapless. M1c adds a resampler so the device format
                    // stays fixed; until then, stop cleanly rather than emit
                    // garbage at the wrong rate.
                    if (nextFormat.sampleRate != format_.sampleRate ||
                        nextFormat.channels != format_.channels) {
                        closeTrack();
                        break;
                    }

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
    std::uint64_t start = 0;
    {
        std::lock_guard lock(seamMutex_);
        start = audibleTrackStart_;
    }
    const std::uint64_t played = output_.framesPlayed();
    return (played > start) ? static_cast<double>(played - start) / rate : 0.0;
}

}  // namespace xpcog
