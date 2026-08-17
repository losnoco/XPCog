// miniaudio backend for IAudioOutput.
//
// The data callback runs on a real-time thread. Its entire body is: read from the
// ring, apply an atomic gain, zero any tail it could not fill. No lock, no
// allocation, no std::function, no logging, no system call. Device lifecycle
// (init/uninit/reconfigure) happens on the caller's thread, never here.

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <miniaudio.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace xpcog {
namespace {

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
        deviceConfig.playback.format   = ma_format_f32;
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

        if (ma_device_init(&context_, &deviceConfig, &device_) != MA_SUCCESS) {
            return false;
        }
        deviceValid_ = true;

        format_.sampleRate    = device_.sampleRate;
        format_.channels      = device_.playback.channels;
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(device_.playback.channels);

        framesPlayed_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);

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

    void pause() override {
        std::lock_guard lock(deviceMutex_);
        if (deviceValid_) {
            ma_device_stop(&device_);
        }
    }

    void resume() override {
        std::lock_guard lock(deviceMutex_);
        if (deviceValid_) {
            ma_device_start(&device_);
        }
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

    void setDeviceInvalidatedCallback(std::function<void()> callback) override {
        std::lock_guard lock(callbackMutex_);
        onInvalidated_ = std::move(callback);
    }

private:
    // --- real-time path ---------------------------------------------------

    static void dataCallback(ma_device* device, void* output, const void* /*input*/,
                             ma_uint32 frameCount) {
        auto* self = static_cast<MiniaudioOutput*>(device->pUserData);
        auto* out  = static_cast<float*>(output);

        const std::size_t wanted =
            static_cast<std::size_t>(frameCount) * device->playback.channels;

        const std::size_t got = self->sink_.read(out, wanted);

        if (got < wanted) {
            // Silence the tail rather than repeating stale samples.
            std::memset(out + got, 0, (wanted - got) * sizeof(float));
            self->underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        const float gain = self->volume_.load(std::memory_order_relaxed);
        if (gain != 1.0F) {
            for (std::size_t i = 0; i < got; ++i) {
                out[i] *= gain;
            }
        }

        // The playback clock. Track changes are announced against this, so a seam
        // is reported when it is audible rather than when it was decoded.
        self->framesPlayed_.fetch_add(frameCount, std::memory_order_relaxed);
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
    AudioFormat        format_{};

    std::mutex            callbackMutex_;
    std::function<void()> onInvalidated_;

    std::atomic<float>         volume_{1.0F};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> framesPlayed_{0};
};

}  // namespace

std::unique_ptr<IAudioOutput> makeMiniaudioOutput(RingBuffer& sink) {
    return std::make_unique<MiniaudioOutput>(sink);
}

}  // namespace xpcog
