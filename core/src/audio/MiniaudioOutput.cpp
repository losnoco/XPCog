// miniaudio backend for IAudioOutput.
//
// The data callback runs on a real-time thread. Its entire body is: read from the
// ring, zero any tail it could not fill, apply the volume and transport fade, and
// copy the result to the visualiser tap if one is attached. No lock, no allocation,
// no std::function, no logging, no system call. Device lifecycle
// (init/uninit/reconfigure) happens on the caller's thread, never here.
//
// That list is a promise, and it is kept deliberately short -- so anything added to
// it gets named here rather than appearing quietly. The tap is the most recent
// addition and it is the last step on purpose: it must see what the speakers get,
// which means after the gain, not before.
//
// One step has since joined it: converting to an integer device format, when the
// device was opened in one. It is still no lock, no allocation and no system
// call -- the scratch buffer it needs is sized once, in start(), and a callback
// asking for more frames than it holds is served in chunks rather than by
// growing it. See the callback.

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"
#include "xpcog/core/audio/TransportGain.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

/// The device layouts, and only those. Anything else -- U8, S8, F64, the DSD
/// that arrives from a decoder -- is not something a device is opened in here,
/// and asking for one falls back to float rather than failing to play.
[[nodiscard]] ma_format toMaFormat(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::S16: return ma_format_s16;
        case SampleFormat::S24: return ma_format_s24;
        case SampleFormat::S32: return ma_format_s32;
        default: return ma_format_f32;
    }
}

/// What the device actually ended up carrying. Asked of the device rather than
/// assumed from the request, because miniaudio negotiates: a device that will
/// not do s24 is opened in something it will.
[[nodiscard]] SampleFormat fromMaFormat(ma_format format) noexcept {
    switch (format) {
        case ma_format_u8: return SampleFormat::U8;
        case ma_format_s16: return SampleFormat::S16;
        case ma_format_s24: return SampleFormat::S24;
        case ma_format_s32: return SampleFormat::S32;
        default: return SampleFormat::F32;
    }
}

[[nodiscard]] std::uint32_t bitsFor(SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::U8: return 8;
        case SampleFormat::S16: return 16;
        case SampleFormat::S24: return 24;
        default: return 32;
    }
}

class MiniaudioOutput final : public IAudioOutput {
public:
    explicit MiniaudioOutput(RingBuffer& sink) : sink_(sink) {}

    ~MiniaudioOutput() override {
        MiniaudioOutput::stop();
        if (contextValid_) {
            ma_context_uninit(&context_);
        }
    }

    bool start(const Config& config) override {
        std::lock_guard lock(deviceMutex_);
        stopLocked();

        if (!ensureContextLocked()) {
            return false;
        }

        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = toMaFormat(config.format);
        deviceConfig.playback.channels = config.channels;
        deviceConfig.sampleRate        = static_cast<ma_uint32>(config.sampleRate);
        deviceConfig.periodSizeInFrames = config.bufferFrames;
        deviceConfig.dataCallback      = &MiniaudioOutput::dataCallback;
        deviceConfig.notificationCallback = &MiniaudioOutput::notificationCallback;
        deviceConfig.pUserData            = this;

        ma_device_id  deviceId{};
        if (!config.deviceId.empty() && resolveDeviceIdLocked(config.deviceId, deviceId)) {
            deviceConfig.playback.pDeviceID = &deviceId;
        }

        // Exclusive is asked for, not demanded. WASAPI grants it only when
        // nothing else holds the device, backends with no such concept return
        // MA_SHARE_MODE_NOT_SUPPORTED, and either way refusing to play would be
        // the wrong answer to "I would rather not be resampled".
        exclusiveHeld_ = false;
        if (config.exclusive) {
            deviceConfig.playback.shareMode = ma_share_mode_exclusive;
            if (ma_device_init(&context_, &deviceConfig, &device_) == MA_SUCCESS) {
                exclusiveHeld_ = true;
            } else {
                deviceConfig.playback.shareMode = ma_share_mode_shared;
            }
        }

        if (!exclusiveHeld_ &&
            ma_device_init(&context_, &deviceConfig, &device_) != MA_SUCCESS) {
            return false;
        }
        deviceValid_ = true;

        format_.sampleRate = device_.sampleRate;
        format_.channels   = device_.playback.channels;
        // Read back rather than echoed: a device that refused s24 is running in
        // whatever it accepted, and a caller that asked for exact integers needs
        // to be told it did not get them -- that is the difference between DoP
        // playing and DoP being emitted as noise.
        format_.format        = fromMaFormat(device_.playback.format);
        format_.bitsPerSample = bitsFor(format_.format);
        format_.channelConfig = guessChannelConfig(device_.playback.channels);

        // The scratch the callback converts through, sized once here so that it
        // never allocates. miniaudio asks for at most a period at a time; the
        // callback chunks anyway rather than trusting that, because being wrong
        // about it would be a buffer overrun on a real-time thread.
        //
        // Not allocated at all for a float device, where the callback reads the
        // ring straight into the device buffer as it always has.
        if (format_.format == SampleFormat::F32) {
            scratch_.clear();
            scratch_.shrink_to_fit();
        } else {
            const std::size_t frames =
                device_.playback.internalPeriodSizeInFrames > 0
                    ? device_.playback.internalPeriodSizeInFrames
                    : 4096;
            scratch_.assign(frames * device_.playback.channels, 0.0F);
        }

        framesPlayed_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
        // A fresh device is not a paused one. Without this, a stop() taken while
        // paused-and-held would leave the flag set and the next track would open
        // a device that plays silence -- a player that appears to start and emits
        // nothing, with the clock frozen to match.
        silenced_.store(false, std::memory_order_release);

        // Before the device runs, and this is not housekeeping. A faded stop
        // leaves the level at zero, and the engine's play() calls stop() first --
        // so without this, every track after the first played to a gain of zero.
        // The old code got away with it only because its callback skipped the
        // multiply once the ramp had settled, which is the bug next door.
        fade_.reset();

        if (ma_device_start(&device_) != MA_SUCCESS) {
            ma_device_uninit(&device_);
            deviceValid_ = false;
            return false;
        }
        return true;
    }

    void stop() override {
        std::lock_guard lock(deviceMutex_);
        stopLocked();
    }

    void setSuspendOnPause(bool suspend) override {
        suspendOnPause_.store(suspend, std::memory_order_relaxed);
    }

    void pause() override {
        std::lock_guard lock(deviceMutex_);
        if (!deviceValid_) {
            return;
        }
        if (suspendOnPause_.load(std::memory_order_relaxed)) {
            silenced_.store(false, std::memory_order_release);
            ma_device_stop(&device_);
            return;
        }
        // Held rather than handed back. The device keeps running and the callback
        // emits silence, so the driver never loses its stream -- which is the
        // point for an exclusive device another application would take in the
        // gap, or a DAC that clicks every time the stream stops.
        silenced_.store(true, std::memory_order_release);
    }

    void resume() override {
        std::lock_guard lock(deviceMutex_);
        if (!deviceValid_) {
            return;
        }
        // Cleared first: the device may never have stopped, in which case the
        // callback is running right now and this flag is the only thing between
        // it and the audio.
        silenced_.store(false, std::memory_order_release);
        // Harmless on a device that was never stopped -- miniaudio answers
        // MA_INVALID_OPERATION and changes nothing -- so the two pause modes
        // need no bookkeeping here to tell them apart.
        ma_device_start(&device_);
    }

    [[nodiscard]] AudioFormat negotiatedFormat() const override { return format_; }

    [[nodiscard]] double latencySeconds() const override {
        if (!deviceValid_ || format_.sampleRate <= 0.0) {
            return 0.0;
        }
        const double frames = device_.playback.internalPeriodSizeInFrames *
                              device_.playback.internalPeriods;
        return frames / format_.sampleRate;
    }

    [[nodiscard]] bool exclusiveHeld() const override {
        std::lock_guard lock(deviceMutex_);
        return deviceValid_ && exclusiveHeld_;
    }

    [[nodiscard]] double preferredSampleRate(std::string_view deviceId) const override {
        std::lock_guard lock(deviceMutex_);
        auto*           self = const_cast<MiniaudioOutput*>(this);
        if (!self->ensureContextLocked()) {
            return 0.0;
        }

        // The device about to be opened, which is not necessarily the default
        // one. A null id is what miniaudio takes to mean the default, so an
        // empty selection and a selection that no longer resolves both land
        // there -- the latter deliberately, since that is also what start()
        // does with an id it cannot find.
        ma_device_id  resolved{};
        ma_device_id* which = nullptr;
        if (!deviceId.empty() &&
            self->resolveDeviceIdLocked(std::string{deviceId}, resolved)) {
            which = &resolved;
        }

        ma_device_info info{};
        if (ma_context_get_device_info(&self->context_, ma_device_type_playback,
                                       which, &info) != MA_SUCCESS) {
            return 0.0;
        }
        // The first native format is the one the backend is actually running --
        // on a shared-mode WASAPI device that is the mix format, which is what
        // the listener has set in Windows.
        for (ma_uint32 i = 0; i < info.nativeDataFormatCount; ++i) {
            if (info.nativeDataFormats[i].sampleRate != 0) {
                return static_cast<double>(info.nativeDataFormats[i].sampleRate);
            }
        }
        return 0.0;
    }

    [[nodiscard]] std::vector<DeviceInfo> devices() const override {
        std::lock_guard lock(deviceMutex_);
        auto*           self = const_cast<MiniaudioOutput*>(this);
        if (!self->ensureContextLocked()) {
            return {};
        }

        ma_device_info* playback = nullptr;
        ma_uint32       count    = 0;
        if (ma_context_get_devices(&self->context_, &playback, &count, nullptr,
                                   nullptr) != MA_SUCCESS) {
            return {};
        }

        std::vector<DeviceInfo> out;
        out.reserve(count);
        for (ma_uint32 i = 0; i < count; ++i) {
            out.push_back(DeviceInfo{encodeDeviceId(playback[i].id),
                                     playback[i].name,
                                     playback[i].isDefault != 0});
        }
        return out;
    }

    void rampGain(float target, double milliseconds) override {
        fade_.rampTo(target, milliseconds, negotiatedFormat().sampleRate);
    }

    [[nodiscard]] bool ramping() const override { return fade_.ramping(); }

    void setVolume(float gain) override {
        volume_.store(gain, std::memory_order_relaxed);
    }

    [[nodiscard]] float volume() const override {
        return volume_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t underrunCount() const override {
        return underruns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t framesPlayed() const override {
        return framesPlayed_.load(std::memory_order_relaxed);
    }

    void setTap(AudioTap* tap) override {
        tap_.store(tap, std::memory_order_relaxed);
    }

    void setDeviceInvalidatedCallback(std::function<void()> callback) override {
        std::lock_guard lock(callbackMutex_);
        onInvalidated_ = std::move(callback);
    }

private:
    // --- real-time path ---------------------------------------------------

    static void dataCallback(ma_device* device, void* output, const void* /*input*/,
                             ma_uint32 frameCount) {
        auto*             self     = static_cast<MiniaudioOutput*>(device->pUserData);
        const std::size_t channels = device->playback.channels;

        // Paused, on a device we chose not to hand back. Silence, and nothing
        // else: the ring is not drained, so playback resumes on the sample it
        // stopped on, and framesPlayed_ does not advance, because that counter is
        // the playback clock every track change is timed against. Counting
        // silence into it would walk the seek bar forward through a pause.
        if (self->silenced_.load(std::memory_order_acquire)) {
            std::memset(output, 0,
                        static_cast<std::size_t>(frameCount) * channels *
                            ma_get_bytes_per_sample(device->playback.format));
            return;
        }

        if (device->playback.format == ma_format_f32) {
            // The float path, unchanged: the ring holds float32, so the device
            // buffer is filled in place with nothing between the two.
            self->fill(static_cast<float*>(output),
                       static_cast<std::size_t>(frameCount) * channels, channels);
            self->framesPlayed_.fetch_add(frameCount, std::memory_order_relaxed);
            return;
        }

        // An integer device. Same work, then a conversion -- through a scratch
        // buffer sized in start(), in chunks of whole frames so that a callback
        // larger than the scratch is served correctly rather than truncated.
        //
        // Whole frames matter: the gain and the tap both take a channel count and
        // would be wrong about which channel a sample belonged to if a chunk
        // split one.
        const SampleFormat format = self->format_.format;
        const std::size_t  bytesPerSample = self->format_.bytesPerSample();
        auto*              bytes          = static_cast<std::byte*>(output);

        const std::size_t chunkFrames = channels > 0 ? self->scratch_.size() / channels : 0;
        if (chunkFrames == 0) {
            // Cannot happen -- start() sizes the scratch whenever the device is
            // not float -- but silence is the only safe answer on this thread.
            std::memset(output, 0, static_cast<std::size_t>(frameCount) *
                                       channels * bytesPerSample);
            return;
        }

        std::size_t remaining = frameCount;
        while (remaining > 0) {
            const std::size_t frames  = std::min(remaining, chunkFrames);
            const std::size_t samples = frames * channels;

            self->fill(self->scratch_.data(), samples, channels);
            convertFromFloat32(std::span<const float>{self->scratch_.data(), samples},
                               format,
                               std::span<std::byte>{bytes, samples * bytesPerSample});

            bytes += samples * bytesPerSample;
            remaining -= frames;
        }

        // The playback clock. Track changes are announced against this, so a seam
        // is reported when it is audible rather than when it was decoded.
        self->framesPlayed_.fetch_add(frameCount, std::memory_order_relaxed);
    }

    /// Ring to float buffer, with the tail silenced, the gain applied and the tap
    /// fed. Everything the callback did before a device format came into it.
    ///
    /// Shared by both paths rather than duplicated, because the two differ only
    /// in where the floats end up -- and a second copy of the gain-then-tap order
    /// is exactly the divergence TransportGain.hpp was written about.
    void fill(float* out, std::size_t wanted, std::size_t channels) {
        const std::size_t got = sink_.read(out, wanted);

        if (got < wanted) {
            // Silence the tail rather than repeating stale samples.
            std::memset(out + got, 0, (wanted - got) * sizeof(float));
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        // Volume and the transport fade are separate multipliers -- a fade must not
        // read or overwrite what the user set -- and both are applied by
        // TransportGain, which is shared with OfflineOutput. It used to be written
        // out here, and having a second copy of it in the test double is how the
        // two came to disagree; see TransportGain.hpp.
        fade_.apply(out, got, channels, volume_.load(std::memory_order_relaxed));

        // After the gain, so the visualiser sees what the speakers get -- including
        // a fade, which is the point of tapping here rather than upstream.
        if (AudioTap* tap = tap_.load(std::memory_order_relaxed); tap != nullptr) {
            tap->write(out, got, channels);
        }
    }

    /// Fires on a miniaudio-internal thread, not the RT thread. It only hands the
    /// event onward; reconfiguration happens on whichever thread owns the output.
    static void notificationCallback(const ma_device_notification* notification) {
        if (notification == nullptr || notification->pDevice == nullptr) {
            return;
        }
        if (notification->type != ma_device_notification_type_stopped) {
            return;
        }

        auto* self = static_cast<MiniaudioOutput*>(notification->pDevice->pUserData);

        std::function<void()> callback;
        {
            std::lock_guard lock(self->callbackMutex_);
            callback = self->onInvalidated_;
        }
        if (callback) {
            callback();
        }
    }

    // --- device lifecycle, caller's thread --------------------------------

    bool ensureContextLocked() {
        if (contextValid_) {
            return true;
        }
        if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
            return false;
        }
        contextValid_ = true;
        return true;
    }

    void stopLocked() {
        if (deviceValid_) {
            ma_device_uninit(&device_);
            deviceValid_ = false;
        }
    }

    /// ma_device_id is an opaque union, so it is carried as hex rather than
    /// assuming it holds a printable string on every backend.
    [[nodiscard]] static std::string encodeDeviceId(const ma_device_id& id) {
        static constexpr char kHex[] = "0123456789abcdef";
        const auto*           bytes  = reinterpret_cast<const unsigned char*>(&id);

        std::string out;
        out.reserve(sizeof(id) * 2);
        for (std::size_t i = 0; i < sizeof(id); ++i) {
            out.push_back(kHex[bytes[i] >> 4]);
            out.push_back(kHex[bytes[i] & 0x0F]);
        }
        return out;
    }

    bool resolveDeviceIdLocked(const std::string& encoded, ma_device_id& out) {
        ma_device_info* playback = nullptr;
        ma_uint32       count    = 0;
        if (ma_context_get_devices(&context_, &playback, &count, nullptr, nullptr) !=
            MA_SUCCESS) {
            return false;
        }
        for (ma_uint32 i = 0; i < count; ++i) {
            if (encodeDeviceId(playback[i].id) == encoded) {
                out = playback[i].id;
                return true;
            }
        }
        return false;
    }

    RingBuffer& sink_;

    mutable std::mutex deviceMutex_;
    ma_context         context_{};
    ma_device          device_{};
    bool               contextValid_ = false;
    bool               deviceValid_  = false;
    /// Whether the running device was granted exclusively. Not what was asked
    /// for -- what was given.
    bool               exclusiveHeld_ = false;
    AudioFormat        format_{};

    /// Whether pause() releases the device or holds it. Cog's
    /// `suspendOutputOnPause`; see IAudioOutput::setSuspendOnPause().
    std::atomic<bool> suspendOnPause_{true};

    /// Set while paused on a device that was held. Read by the callback on every
    /// buffer, which is why it is atomic rather than guarded by deviceMutex_ --
    /// the real-time thread may not take a lock.
    std::atomic<bool> silenced_{false};

    /// Float staging for an integer device, sized in start() and never resized
    /// afterwards -- the callback runs on a real-time thread and must not
    /// allocate. Empty when the device carries float, where nothing stages.
    std::vector<float> scratch_;

    std::mutex            callbackMutex_;
    std::function<void()> onInvalidated_;

    std::atomic<float>         volume_{1.0F};

    /// The transport fade. Owns its own ramp state; see TransportGain.hpp.
    TransportGain              fade_;
    /// Borrowed, and read by the callback -- hence atomic, so the visualiser can be
    /// switched on and off without stopping playback.
    std::atomic<AudioTap*>     tap_{nullptr};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> framesPlayed_{0};
};

}  // namespace

std::vector<DeviceInfo> enumerateOutputDevices() {
    // Through a throwaway output rather than a second copy of the context
    // handling: the ring is never touched, because nothing is started.
    RingBuffer unused{1};
    return makeMiniaudioOutput(unused)->devices();
}

std::unique_ptr<IAudioOutput> makeMiniaudioOutput(RingBuffer& sink) {
    return std::make_unique<MiniaudioOutput>(sink);
}

}  // namespace xpcog
