#include "xpcog/core/audio/AudioEngine.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

/// How long the feeder sleeps when the ring is full. Short relative to the ring,
/// so it wakes well before the device runs dry.
constexpr auto kFeederBackoff = std::chrono::milliseconds(2);

/// The deep buffer ahead of the DSP chain -- Cog's BUFFER_SIZE, about three
/// seconds of stereo at 44.1 kHz. This is the depth that keeps the device fed
/// across a scheduling hiccup, and it sits before the chain so that depth does
/// not become DSP latency.
constexpr std::size_t kPreRingSamples = 1U << 18;

/// How much the DSP thread moves per pass. Large enough that the per-call
/// overhead is irrelevant, small enough to stay well inside the shallow ring.
constexpr std::size_t kDspBlockSamples = 4096;

}  // namespace

struct AudioEngine::OpenTrack {
    SourcePtr  source;
    DecoderPtr decoder;
    Url        url;
};

AudioEngine::AudioEngine(const PluginRegistry& registry, IAudioOutput& output,
                         RingBuffer& ring, const Settings& settings)
    : registry_(registry), output_(output), settings_(settings), ring_(ring),
      preRing_(kPreRingSamples) {
    // Order is the signal path. The equaliser sits after conversion, so it always
    // runs at the device's rate and its coefficients survive a track whose source
    // rate differs. The fader is last, so it ramps the finished signal rather than
    // the equaliser's input -- fading before a boost would let the boost undo it.
    chain_.push_back(&equalizer_);
    chain_.push_back(&fader_);
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

    // The decoder's own side of the stream-metadata question. Some formats carry
    // the now-playing title in the audio rather than around it -- an HLS
    // rendition leads every segment with an ID3v2 tag, and a chained Ogg starts a
    // new comment header -- and the source underneath knows nothing about any of
    // it, so polling takeUpdatedMetadata() alone never sees those.
    //
    // A flag rather than calling the delegate from here: this fires from inside
    // readAudio(), and every other delegate call happens at a defined point in
    // the pump rather than part-way through decoding.
    track->decoder->setChangeCallback([this](bool /*propertiesChanged*/,
                                             bool metadataChanged) {
        if (metadataChanged) {
            decoderTagsDirty_.store(true, std::memory_order_release);
        }
    });

    // A track with no known length is a live stream, and nothing has ever opened
    // it before now -- so whatever the decoder read on the way in is news, and
    // without publishing it the row stays named after its URL until the song
    // changes, which on a radio station is minutes away.
    //
    // Only then. A file's tags come from the metadata readers, which are better
    // at it than a decoder is, and republishing the decoder's would overwrite
    // them with a second opinion every time the track started.
    const TrackProperties properties = track->decoder->properties();
    decoderTagsDirty_.store(properties.totalFrames <= 0, std::memory_order_release);

    // What this decoder counts frames in. Everything the engine tracks is in
    // device frames; the decoder's own units only appear at the two ends of a
    // seek, and this is what converts between them.
    trackRate_.store(properties.format.sampleRate, std::memory_order_release);
    {
        const std::lock_guard lock(trackMutex_);
        track_ = std::move(track);
    }
    return true;
}

void AudioEngine::closeTrack() {
    decoderTagsDirty_.store(false, std::memory_order_release);

    std::unique_ptr<OpenTrack> doomed;
    {
        const std::lock_guard lock(trackMutex_);
        doomed = std::move(track_);
    }

    // Destroyed outside the lock: ~HttpSource joins its network thread, and
    // holding the pointer lock across that would block a concurrent
    // interruptTrack() on exactly the teardown it is trying to help along.
    if (doomed) {
        // Decoder first: it borrows the source and must not outlive it.
        doomed->decoder.reset();
        doomed->source.reset();
    }
}

void AudioEngine::interruptTrack() {
    const std::lock_guard lock(trackMutex_);
    if (!track_) {
        return;
    }
    // Both, and in this order: a decoder may be waiting on its own buffering
    // above the source, and unblocking the source first would leave it parked.
    if (track_->decoder) {
        track_->decoder->interrupt();
    }
    if (track_->source) {
        track_->source->interrupt();
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

    // Unless the output will not run it. DSD arrives at 352,800 or 705,600 Hz --
    // one byte per channel per frame, eight one-bit samples in each -- and no
    // current backend opens a device there, so asking fails the device open and
    // the track simply does not play. That was the first shape of DSD playback
    // here, and it failed silently: play() returned false and said nothing.
    //
    // The output is asked rather than told, because which rates are reachable
    // belongs to the backend -- see IAudioOutput::supportsSampleRate. Cog never
    // asks at all: its device keeps its own format and everything is resampled
    // into it (OutputCoreAudio.m, -outputFormatForInputFormat:).
    if (!output_.supportsSampleRate(format_.sampleRate)) {
        const double preferred = output_.preferredSampleRate();
        format_.sampleRate = output_.supportsSampleRate(preferred) ? preferred : 48000.0;
    }

    // FreeSurround is decided here, and only here, for the same reason the rate
    // is: it widens the device from two channels to six, and the device is
    // opened once. A later track that is already multichannel is fitted to
    // stereo and upmixed again rather than passed through -- which is lossy, and
    // is the price of not reconfiguring the device mid-album. Only offered when
    // the first track is stereo, because upmixing something that already has a
    // surround field is not what the control means.
    const bool wantFreeSurround = settings_.EnableFSurround() && format_.channels == 2;
    if (wantFreeSurround) {
        format_.channels      = 6;
        format_.channelConfig = kChannelFrontLeft | kChannelFrontRight |
                                kChannelFrontCenter | kChannelLFE | kChannelBackLeft |
                                kChannelBackRight;
    }

    if (!converter_.setOutputFormat(format_.sampleRate, format_.channels,
                                    settings_.Resampling())) {
        closeTrack();
        return false;
    }
    converter_.setFreeSurround(wantFreeSurround);
    converter_.reset();
    converter_.setHdcdEnabled(settings_.EnableHDCD());
    converter_.setHalveDsd(settings_.HalveDsdVolume());
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

    // Both stages start empty. The caller's ring is the shallow one the device
    // drains; the deep one is ours.
    ring_.clear();
    preRing_.clear();

    framesWritten_ = 0;
    pendingDeviceSwitch_.store(false, std::memory_order_relaxed);
    deviceLost_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(seamMutex_);
        pendingSeams_.clear();
        seekPlayedBase_ = 0;
        seekTrackBase_  = 0;
        audibleUrl_        = url;
        audibleTrackStart_ = 0;
        // A fresh device, counting from zero, and no earlier one behind it.
        deviceFramesBase_ = 0;
    }
    {
        std::lock_guard lock(finishedMutex_);
        finished_ = false;
    }

    IAudioOutput::Config config;
    config.sampleRate = format_.sampleRate;
    config.channels   = format_.channels;
    config.deviceId   = chosenDeviceId();
    config.exclusive  = settings_.OutputExclusive();

    // Fill the deep buffer before the device starts. Otherwise the first
    // callbacks land before the feeder has produced anything and are counted as
    // underruns -- real ones, audible as a click at the start of playback.
    {
        AudioChunk        chunk;
        const std::size_t target = preRing_.capacity() / 2;
        while (preRing_.availableToRead() < target &&
               track_->decoder->readAudio(chunk)) {
            converted_.clear();
            if (!converter_.process(chunk, converted_) || converted_.empty()) {
                continue;
            }
            preRing_.write(converted_.data(), converted_.size());
            framesWritten_ += converted_.size() / format_.channels;
        }
    }

    running_.store(true, std::memory_order_release);
    dsp_ = std::thread([this] { dspLoop(); });

    // And prime the shallow one, for the same reason: the device's first callback
    // must find audio waiting. The pump runs hundreds of times faster than
    // playback, so this is microseconds rather than a stall -- but it is bounded,
    // because a track shorter than the shallow ring would never reach the target.
    for (int spin = 0; spin < 500 && ring_.availableToRead() < ring_.capacity() / 2;
         ++spin) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (!output_.start(config)) {
        running_.store(false, std::memory_order_release);
        if (dsp_.joinable()) {
            dsp_.join();
        }
        closeTrack();
        return false;
    }

    // What the running device was asked for, so a later request can be told
    // apart from the same one arriving again -- a settings write that resolves
    // to the device already playing must not interrupt it.
    openDeviceId_  = config.deviceId;
    openExclusive_ = config.exclusive;

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
    fader_.setEnabled(settings_.EnableFading());
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

void AudioEngine::dspLoop() {
    std::vector<float> block(kDspBlockSamples);
    const auto         channels = static_cast<std::size_t>(format_.channels);
    std::uint64_t      seenEpoch = flushEpoch_.load(std::memory_order_acquire);

    while (running_.load(std::memory_order_acquire)) {
        // Settings first, so a slider moved during the wait below is already in
        // the coefficients by the time the next block is filtered.
        if (dspDirty_.exchange(false, std::memory_order_relaxed)) {
            applyDspSettings();
        }

        if (const std::uint64_t epoch = flushEpoch_.load(std::memory_order_acquire);
            epoch != seenEpoch) {
            // The feeder has dropped the pre-seek audio upstream. Two things
            // still hold the old position: the filter state, and whatever this
            // thread already handed to the device.
            //
            // Only the filter state is this thread's to drop. The discard itself
            // is requested by the feeder, which is also the only place the ring's
            // fill can be read before it happens -- see performSeek(). Asking for
            // it again here would re-arm a flush the device may already have
            // acknowledged, and take the *post*-seek audio with it.
            for (DSPNode* node : chain_) {
                node->reset();
            }
            seenEpoch = epoch;
        }

        // A pending upstream flush is consumed here explicitly rather than as a
        // side effect of a normal read. Only a read clears it, so leaving it to
        // the availability check below would deadlock whenever the flush arrived
        // with the deep ring already empty: nothing to read, so no read, so the
        // flag never clears and the feeder waits on it forever.
        if (preRing_.flushPending()) {
            preRing_.read(block.data(), block.size());
            std::this_thread::sleep_for(kFeederBackoff);
            continue;
        }

        // Nothing new may be written until the device has acknowledged its own
        // discard, or it would take the post-seek audio with it.
        if (ring_.flushPending()) {
            std::this_thread::sleep_for(kFeederBackoff);
            continue;
        }

        // Whole frames only. read() hands back whatever is available, and a
        // block that ended mid-frame would shift every channel by one from there
        // on -- silent, and it would sound like the stereo image collapsing.
        // Claimed *before* the ring is even measured, and released only once the
        // block has been handed downstream. The order is the whole point: what
        // makes it safe is that this store is sequenced before the read's own
        // release store, so a feeder that sees the samples gone from preRing_ is
        // guaranteed to see this flag set. See the declaration for what the
        // feeder does with it.
        dspBusy_.store(true, std::memory_order_relaxed);

        const std::size_t available = preRing_.availableToRead();
        const std::size_t wanted    = std::min(available, block.size()) / channels * channels;
        if (wanted == 0) {
            dspBusy_.store(false, std::memory_order_release);
            std::this_thread::sleep_for(kFeederBackoff);
            continue;
        }

        const std::size_t got = preRing_.read(block.data(), wanted);
        if (got == 0) {
            dspBusy_.store(false, std::memory_order_release);
            continue;  // a flush landed between the check and the read
        }

        const std::size_t frames = got / channels;
        for (DSPNode* node : chain_) {
            if (node->active()) {
                node->process(block.data(), frames);
            }
        }

        std::size_t written = 0;
        while (written < got && running_.load(std::memory_order_acquire)) {
            written += ring_.write(block.data() + written, got - written);
            if (written < got) {
                std::this_thread::sleep_for(kFeederBackoff);
            }
        }

        // Released only here, after the write. A release store, so a feeder that
        // observes this as false also observes everything written to ring_ above.
        dspBusy_.store(false, std::memory_order_release);
    }
}

bool AudioEngine::writeSamples(const float* samples, std::size_t count) {
    std::size_t written = 0;
    while (written < count) {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        written += preRing_.write(samples + written, count - written);
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

        // A pause requested with fading on stops the device here, once the ramp
        // has reached silence. Doing it in pause() would silence the fade instead
        // of playing it.
        if (pendingPause_.load(std::memory_order_acquire) && !output_.ramping()) {
            output_.pause();
            pendingPause_.store(false, std::memory_order_release);
        }

        // Before the seek, so a device change and a seek arriving together are
        // applied in the order that leaves the shorter gap: the switch keeps
        // what is queued, and the seek then throws it away. The other order
        // discards the queue and then stops the device that was about to be
        // refilled.
        if (pendingDeviceSwitch_.exchange(false, std::memory_order_acq_rel)) {
            performDeviceSwitch();
            if (deviceLost_.load(std::memory_order_acquire)) {
                break;
            }
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
        // Both stages: the deep one until the pump has dropped it, the shallow
        // one until the device has. Waiting only on the first would re-base the
        // position while up to a shallow ring of old audio was still queued,
        // which is the error the seek-position work exists to avoid.
        if (preRing_.flushPending() || ring_.flushPending()) {
            std::this_thread::sleep_for(kFeederBackoff);
            continue;
        }

        if (seekBasePending_) {
            // The stale audio is gone, so what the device reports having played
            // is now the true starting point for the new position.
            std::lock_guard lock(seamMutex_);
            seekPlayedBase_  = totalFramesPlayedLocked();
            seekTrackBase_   = pendingSeekTrack_;
            framesWritten_   = seekPlayedBase_;
            seekBasePending_ = false;
        }

        if (!track_ || !track_->decoder->readAudio(chunk)) {
            // A read that ended because stop() interrupted it is not the end of
            // the track, and asking the delegate for the next one here would
            // open a fresh source during shutdown -- for an HTTP URL, a whole
            // new connection nothing will ever close.
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            // End of the current decoder. Ask for the next track *now*, while the
            // audio already in the ring is still playing out -- this is what makes
            // the handoff gapless. Cog does the same in -endOfInputReached:.
            std::optional<Url> next =
                (delegate_ != nullptr) ? delegate_->nextTrack() : std::nullopt;

            // URLs already tried and failed during *this* advance. The delegate
            // answers from the playlist's repeat and shuffle rules, so with
            // repeat on and every remaining entry undecodable it hands back the
            // same URLs for ever -- a single bad file on repeat-one is the
            // simplest case. Seeing a failed candidate come round again means
            // the delegate has no fresh answer left, and the honest outcome is
            // to stop, not to spin at full speed emitting trackFailed.
            std::vector<std::string> failedThisAdvance;

            bool advanced = false;
            while (next.has_value()) {
                const Url candidate = *next;

                const std::string key = candidate.toString();
                if (std::find(failedThisAdvance.begin(), failedThisAdvance.end(),
                              key) != failedThisAdvance.end()) {
                    break;
                }

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

                failedThisAdvance.push_back(key);
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

        pollStreamMetadata();

        if (!writeToRing(chunk)) {
            break;
        }
    }

    // Flush the resampler's delay line, or the last fraction of a second of the
    // final track is silently dropped.
    //
    // Not when the device is gone, though: writeSamples() blocks until there is
    // room, and with nothing draining the rings there never will be. The tail
    // of a track is not worth deadlocking the feeder for, and it has nowhere to
    // go in any case.
    if (running_.load(std::memory_order_acquire) &&
        !deviceLost_.load(std::memory_order_acquire)) {
        converted_.clear();
        converter_.drain(converted_);
        if (!converted_.empty()) {
            writeSamples(converted_.data(), converted_.size());
        }
    }

    // Let the device play out what is still buffered before declaring the end.
    //
    // Three places hold audio, not two. Between the rings sits the DSP thread's
    // own block, and a block in flight is counted by neither -- so the obvious
    // condition declares the end while up to one block is still on its way to the
    // device, and stop() then tears down before it arrives. That lost the last
    // few milliseconds of every track: inaudible enough to survive listening
    // tests, and caught by a test that renders the same tone twice and compares
    // the lengths, where it showed up as a capture short by exactly one block.
    // deviceLost_ short-circuits it: with nothing draining the ring, "wait for
    // the buffers to empty" is a wait for something that cannot happen.
    while (running_.load(std::memory_order_acquire) &&
           !deviceLost_.load(std::memory_order_acquire) &&
           (preRing_.availableToRead() > 0 ||
            dspBusy_.load(std::memory_order_acquire) ||
            ring_.availableToRead() > 0)) {
        // Here as well as above, and this is not belt and braces. Decoding runs
        // hundreds of times faster than playback, so a track shorter than the
        // deep ring is fully decoded within moments of starting and the feeder
        // spends nearly all of its life right here -- which made a device
        // change requested during an ordinary song a request that was simply
        // never read. There is still a device delivering audio; it is still
        // just as switchable.
        if (pendingDeviceSwitch_.exchange(false, std::memory_order_acq_rel)) {
            performDeviceSwitch();
            if (deviceLost_.load(std::memory_order_acquire)) {
                break;
            }
        }
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

void AudioEngine::pollStreamMetadata() {
    // Cog asks the source for this inside each decoder, downcasting to
    // HTTPSource with NSClassFromString -- the same block of code repeated in
    // five of them, and absent from the rest, so whether a stream's titles
    // appear depends on which decoder happened to claim it. Asking here instead
    // costs one virtual call that answers "nothing" for every file, and works
    // for every decoder including ones written before streams existed.
    if (!track_ || delegate_ == nullptr) {
        return;
    }

    if (track_->source) {
        MetadataMap tags = track_->source->takeUpdatedMetadata();
        if (!tags.empty()) {
            delegate_->streamMetadataChanged(track_->url, tags);
        }
    }

    // And the decoder's, for the formats that carry the title inside the audio.
    // Exchanged rather than read: the callback may fire again while this is
    // being reported, and losing that would strand the newer tags until the one
    // after it.
    if (track_->decoder && decoderTagsDirty_.exchange(false, std::memory_order_acq_rel)) {
        MetadataMap tags = track_->decoder->metadata();
        if (!tags.empty()) {
            delegate_->streamMetadataChanged(track_->url, tags);
        }
    }
}

std::string AudioEngine::chosenDeviceId() const {
    const std::string wanted = settings_.OutputDeviceId();
    if (wanted.empty()) {
        return {};  // the system default, and it stays the system default
    }
    // Enumerating costs a backend context, so it is only done when a device has
    // actually been chosen. The rule itself is resolveOutputDevice(), which is
    // where it can be tested without a sound card.
    return resolveOutputDevice(output_.devices(), wanted, settings_.OutputDeviceName());
}

std::string resolveOutputDevice(const std::vector<DeviceInfo>& devices,
                                std::string_view wantedId, std::string_view wantedName) {
    if (wantedId.empty()) {
        return {};
    }
    for (const DeviceInfo& device : devices) {
        if (device.id == wantedId) {
            return device.id;
        }
    }
    if (!wantedName.empty()) {
        for (const DeviceInfo& device : devices) {
            if (device.name == wantedName) {
                return device.id;
            }
        }
    }
    return {};
}

bool AudioEngine::switchOutputDevice() {
    // Playing, and not merely non-stopped. The feeder is what services this,
    // and a paused feeder is parked inside writeSamples() waiting for room in a
    // ring nothing is draining -- so the request would sit unread until the
    // device came back, which is the one moment it is no longer wanted.
    if (status_.load(std::memory_order_relaxed) != PlaybackStatus::Playing) {
        return false;
    }
    pendingDeviceSwitch_.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::startDeviceForSwitch(const IAudioOutput::Config& config) {
    if (!output_.start(config)) {
        return false;
    }

    // The whole point of switching under the stream is that both rings keep
    // what they hold -- and what they hold was converted for the format that
    // was running. A device that negotiates another rate or another channel
    // count would play it at the wrong pitch or with the channels interleaved
    // wrongly, so it is refused here rather than fed. play() is free to accept
    // whatever it is given, because it converts the track into it; this is not.
    const AudioFormat negotiated = output_.negotiatedFormat();
    if (negotiated.sampleRate != format_.sampleRate ||
        negotiated.channels != format_.channels) {
        output_.stop();
        return false;
    }

    openDeviceId_  = config.deviceId;
    openExclusive_ = config.exclusive;
    return true;
}

void AudioEngine::performDeviceSwitch() {
    IAudioOutput::Config wanted;
    wanted.sampleRate = format_.sampleRate;
    wanted.channels   = format_.channels;
    wanted.deviceId   = chosenDeviceId();
    wanted.exclusive  = settings_.OutputExclusive();

    // Asked for, not granted: a device that refused exclusive mode last time
    // will refuse it again, and comparing against what was granted would tear
    // the stream down once per settings write for no change at all.
    if (wanted.deviceId == openDeviceId_ && wanted.exclusive == openExclusive_) {
        return;
    }

    IAudioOutput::Config previous = wanted;
    previous.deviceId             = openDeviceId_;
    previous.exclusive            = openExclusive_;

    bool switched = false;
    {
        // Held across the stop and the start, which is the window this exists to
        // close. framesPlayed() returns to zero inside start(), and
        // deviceFramesBase_ is what makes up the difference -- so a reader that
        // saw one of them move without the other would read a clock that had
        // jumped to the top of the track and back. See the member's declaration.
        std::lock_guard lock(seamMutex_);

        output_.stop();

        // After the stop, not before: this is how far the old device actually
        // got, and an output is entitled to hand over a last block on its way
        // out. Read first, it would be short by that much for the rest of the
        // track.
        const std::uint64_t delivered = totalFramesPlayedLocked();

        switched = startDeviceForSwitch(wanted);
        if (!switched && !startDeviceForSwitch(previous)) {
            // Back to the device that was working was the second attempt, and it
            // ran this very format a moment ago -- so reaching here means the
            // hardware went away underneath us and there is nothing left to play
            // through.
            deviceLost_.store(true, std::memory_order_release);
            return;
        }

        // Either way a device restarted and its counter is back at zero, so the
        // base takes over what it used to report. Staying on the old device is
        // not staying on the old clock.
        deviceFramesBase_ = delivered;
    }

    // Outside the lock, as every other delegate call is: the delegate runs
    // application code, and application code is entitled to ask this engine
    // where it has got to.
    if (!switched && delegate_ != nullptr) {
        delegate_->outputSwitchFailed();
    }
}

std::uint64_t AudioEngine::totalFramesPlayedLocked() const {
    return deviceFramesBase_ + output_.framesPlayed();
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

    // The chain's own state is the DSP thread's to drop -- a biquad holding two
    // samples from the old position rings them into the new one, which is the
    // click a user hears at precisely the moment they are listening for the seek
    // to land. The epoch is published *before* the flush flags so that a pump
    // iteration which observes either flag is guaranteed to observe the epoch as
    // well, and therefore cannot write a block through un-reset filters.
    // Before either flush: this is the audio the device still has queued, which
    // is exactly what the fader's fade out is made of. Once the flush is
    // requested the count is on its way to zero.
    if (format_.channels > 0) {
        fader_.noteDiscardedFrames(ring_.availableToRead() / format_.channels);
    }

    flushEpoch_.fetch_add(1, std::memory_order_release);
    ring_.requestFlush();
    preRing_.requestFlush();

    // The base cannot be taken here. Everything still in the ring is about to be
    // *discarded*, so those frames are never delivered and framesPlayed() will
    // never account for them -- adding them to the base put it up to a ring
    // ahead of reality, and with a three-second ring the clock reported the old
    // position for three seconds after every seek. It is taken below instead,
    // once the consumer has acknowledged the flush and framesPlayed() is
    // truthful again.
    // Back into device frames, which is what the position clock is measured in
    // -- `reached` is the decoder's answer in the decoder's own units. For
    // everything but DSD the ratio is 1.
    const double trackRate = trackRate_.load(std::memory_order_acquire);
    const double scale     = (trackRate > 0.0 && format_.sampleRate > 0.0)
                                 ? format_.sampleRate / trackRate
                                 : 1.0;
    pendingSeekTrack_ =
        static_cast<std::uint64_t>(static_cast<double>(reached) * scale);
    seekBasePending_  = true;
}

bool AudioEngine::seek(double seconds) {
    if (status_.load(std::memory_order_relaxed) == PlaybackStatus::Stopped) {
        return false;
    }
    // The *decoder's* rate, not the device's. IDecoder::seek() counts in the
    // frames the decoder produces, and for DSD that is 705,600 a second against
    // a device running at 48,000 -- so using the device's rate here asked for a
    // position fourteen times too early, which is what "the seeking is way off"
    // looked like. Every PCM file has the two rates equal, which is why this
    // survived until now.
    const double rate = trackRate_.load(std::memory_order_acquire);
    if (rate <= 0.0) {
        return false;
    }

    const double clamped = (seconds > 0.0) ? seconds : 0.0;
    pendingSeek_.store(static_cast<std::int64_t>(clamped * rate),
                       std::memory_order_release);
    return true;
}

void AudioEngine::publishSeams() {
    for (;;) {
        Url became;
        {
            std::lock_guard lock(seamMutex_);
            // Inside the lock, because a device switch moves the base and the
            // device's own counter together and only under this lock are the two
            // consistent. Read outside it, a switch landing in between could put
            // a seam's position behind the clock and announce the next track
            // while the current one was still playing.
            const std::uint64_t played = totalFramesPlayedLocked();
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
    // Fade before tearing anything down, or the last thing heard is a step to
    // silence. Bounded and short: stop() already blocks to join two threads, so
    // waiting out a 200 ms ramp is in keeping -- but it must never hang, hence the
    // deadline rather than a bare "while ramping".
    if (status() == PlaybackStatus::Playing && fadeMilliseconds() > 0.0) {
        const double fade = fadeMilliseconds();
        output_.rampGain(0.0F, fade);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  static_cast<int>(fade) + 50);
        while (output_.ramping() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    pendingPause_.store(false, std::memory_order_release);

    running_.store(false, std::memory_order_release);

    // Before the join, not after. A blocked read never observes running_, and
    // the thing that would end it -- closeTrack() destroying the source -- runs
    // below, on the far side of this join. Without this, stopping a live stream
    // whose server has gone quiet waits for audio that is never coming, which
    // froze the application on every attempt to switch tracks.
    interruptTrack();

    if (feeder_.joinable()) {
        feeder_.join();
    }
    if (dsp_.joinable()) {
        dsp_.join();
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

double AudioEngine::fadeMilliseconds() const {
    return settings_.EnableFading() ? Fader::kDefaultFadeMilliseconds : 0.0;
}

void AudioEngine::pause() {
    if (status() != PlaybackStatus::Playing) {
        return;
    }

    const double fade = fadeMilliseconds();
    if (fade <= 0.0) {
        output_.pause();
        status_.store(PlaybackStatus::Paused, std::memory_order_relaxed);
        return;
    }

    // The status changes now and the device stops later, which is the only way to
    // have both a fade and a responsive button: the fade has to be *played* to be
    // heard, so pausing the device here would cut the very audio being faded.
    // The feeder stops it once the ramp has run -- see feederLoop().
    output_.rampGain(0.0F, fade);
    pendingPause_.store(true, std::memory_order_release);
    status_.store(PlaybackStatus::Paused, std::memory_order_relaxed);
}

void AudioEngine::resume() {
    if (status() != PlaybackStatus::Paused) {
        return;
    }

    // Cancel a fade out still in flight before restarting: pausing and resuming
    // quickly must not leave the gain stuck on its way to zero.
    pendingPause_.store(false, std::memory_order_release);

    // The ramp is armed *before* the device restarts, not after. Between a
    // resume() and a rampGain() the callback is already running at whatever gain
    // the pause left behind, and every frame it emits in that window is emitted at
    // the wrong one -- at the start of a fade in, that means audible before the
    // fade it is supposed to begin with.
    output_.rampGain(1.0F, fadeMilliseconds());
    output_.resume();
    status_.store(PlaybackStatus::Playing, std::memory_order_relaxed);
}

void AudioEngine::waitUntilFinished() {
    std::unique_lock lock(finishedMutex_);
    finishedCv_.wait(lock, [this] { return finished_; });
}

void AudioEngine::setVolume(float gain) { output_.setVolume(gain); }
float AudioEngine::volume() const { return output_.volume(); }

double AudioEngine::playedSeconds() const {
    const double rate = format_.sampleRate;
    if (rate <= 0.0) {
        return 0.0;
    }
    std::lock_guard lock(seamMutex_);
    return static_cast<double>(totalFramesPlayedLocked()) / rate;
}

double AudioEngine::trackPositionSeconds() const {
    const double rate = format_.sampleRate;
    if (rate <= 0.0) {
        return 0.0;
    }
    std::uint64_t start      = 0;
    std::uint64_t seekPlayed = 0;
    std::uint64_t seekTrack  = 0;
    std::uint64_t played     = 0;
    {
        std::lock_guard lock(seamMutex_);
        start      = audibleTrackStart_;
        seekPlayed = seekPlayedBase_;
        seekTrack  = seekTrackBase_;
        // With the bases rather than after them. A device switch rewrites the
        // clock's base while the device's own counter returns to zero, and a
        // read that straddled it would pair one with the other and report the
        // track as having jumped back to its start.
        played = totalFramesPlayedLocked();
    }

    // After a seek the track no longer began where the device's frame counter
    // says it did, so the offset is measured from the seek instead. A later
    // track change moves audibleTrackStart_ past the seek base and takes over.
    if (seekPlayed > start && played >= seekPlayed) {
        return static_cast<double>(seekTrack + (played - seekPlayed)) / rate;
    }
    return (played > start) ? static_cast<double>(played - start) / rate : 0.0;
}

}  // namespace xpcog
