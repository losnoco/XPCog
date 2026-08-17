// A deterministic IAudioOutput that captures audio instead of playing it.
//
// This exists so the gapless seam test can run in CI, where there is no audio
// device, and so that a seam can be inspected sample by sample rather than
// listened to. By default it drains the ring as fast as the feeder fills it,
// which makes the whole engine run at full speed with no real-time behaviour to
// flake on.
//
// That default cannot be observed *during* playback, though: a short file is
// gone in the time it takes to decode, so a test that plays one and then looks
// at the engine is racing this thread. An optional pacing multiple slows the
// drain to a chosen multiple of real time for those tests, which turns the race
// into a window wide enough to act in.

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace xpcog {
namespace {

class OfflineOutput final : public IAudioOutput {
public:
    OfflineOutput(RingBuffer& sink, double speedMultiple)
        : sink_(sink), speedMultiple_(speedMultiple) {}

    ~OfflineOutput() override { OfflineOutput::stop(); }

    bool start(const Config& config) override {
        stop();

        format_.sampleRate    = config.sampleRate;
        format_.channels      = config.channels;
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;
        format_.channelConfig = guessChannelConfig(config.channels);

        {
            std::lock_guard lock(mutex_);
            captured_.clear();
        }
        framesPlayed_.store(0, std::memory_order_relaxed);

        running_.store(true, std::memory_order_release);
        drain_ = std::thread([this] { drainLoop(); });
        return true;
    }

    void stop() override {
        running_.store(false, std::memory_order_release);
        if (drain_.joinable()) {
            drain_.join();
        }
    }

    void pause() override { paused_.store(true, std::memory_order_relaxed); }
    void resume() override { paused_.store(false, std::memory_order_relaxed); }

    [[nodiscard]] AudioFormat negotiatedFormat() const override { return format_; }

    /// Zero: there is no device, so a seam becomes "audible" the instant it is
    /// consumed. This is exactly what makes the test deterministic.
    [[nodiscard]] double latencySeconds() const override { return 0.0; }

    [[nodiscard]] std::vector<DeviceInfo> devices() const override {
        return {DeviceInfo{"offline", "Offline capture", true}};
    }

    void setVolume(float gain) override {
        volume_.store(gain, std::memory_order_relaxed);
    }
    [[nodiscard]] float volume() const override {
        return volume_.load(std::memory_order_relaxed);
    }

    /// Always zero. An offline sink cannot underrun -- it waits for the feeder
    /// rather than emitting silence, so counting underruns here would be noise.
    [[nodiscard]] std::uint64_t underrunCount() const override { return 0; }

    [[nodiscard]] std::uint64_t framesPlayed() const override {
        return framesPlayed_.load(std::memory_order_relaxed);
    }

    void setDeviceInvalidatedCallback(std::function<void()>) override {}

    [[nodiscard]] std::vector<float> captured() const {
        std::lock_guard lock(mutex_);
        return captured_;
    }

private:
    void drainLoop() {
        std::vector<float> scratch(4096);

        // Frames this loop is currently entitled to consume, when pacing. It
        // accrues with elapsed time rather than being derived from a fixed start
        // instant, so pausing simply stops it accruing and no catch-up burst is
        // owed on resume.
        double     allowance = 0.0;
        auto       lastTick  = std::chrono::steady_clock::now();
        const bool paced =
            speedMultiple_ > 0.0 && format_.sampleRate > 0 && format_.channels > 0;

        while (running_.load(std::memory_order_acquire)) {
            if (paused_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                lastTick = std::chrono::steady_clock::now();
                continue;
            }

            std::size_t limit = scratch.size();
            if (paced) {
                const auto now = std::chrono::steady_clock::now();
                allowance += std::chrono::duration<double>(now - lastTick).count() *
                             static_cast<double>(format_.sampleRate) * speedMultiple_;
                lastTick = now;

                // Capped at one read. Entitlement otherwise accrues while the
                // ring is empty -- which it is for as long as the feeder takes
                // to open the first decoder -- and is then spent in a single
                // burst, which is the unpaced behaviour this exists to avoid.
                allowance = std::min(allowance,
                                     static_cast<double>(scratch.size() / format_.channels));

                const auto frames = static_cast<std::size_t>(allowance);
                if (frames == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                limit = std::min(limit, frames * format_.channels);
            }

            const std::size_t got = sink_.read(scratch.data(), limit);
            if (got == 0) {
                std::this_thread::yield();
                continue;
            }
            if (paced) {
                allowance -= static_cast<double>(got / format_.channels);
            }

            const float gain = volume_.load(std::memory_order_relaxed);
            {
                std::lock_guard lock(mutex_);
                if (gain == 1.0F) {
                    captured_.insert(captured_.end(), scratch.begin(),
                                     scratch.begin() + static_cast<std::ptrdiff_t>(got));
                } else {
                    for (std::size_t i = 0; i < got; ++i) {
                        captured_.push_back(scratch[i] * gain);
                    }
                }
            }

            if (format_.channels > 0) {
                framesPlayed_.fetch_add(got / format_.channels,
                                        std::memory_order_relaxed);
            }
        }

        // Take whatever the feeder left behind, so the capture is complete.
        for (;;) {
            const std::size_t got = sink_.read(scratch.data(), scratch.size());
            if (got == 0) {
                break;
            }
            std::lock_guard lock(mutex_);
            captured_.insert(captured_.end(), scratch.begin(),
                             scratch.begin() + static_cast<std::ptrdiff_t>(got));
        }
    }

    RingBuffer& sink_;
    const double speedMultiple_;

    mutable std::mutex mutex_;
    std::vector<float> captured_;

    std::thread                drain_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          paused_{false};
    std::atomic<float>         volume_{1.0F};
    std::atomic<std::uint64_t> framesPlayed_{0};
    AudioFormat                format_{};
};

}  // namespace

std::unique_ptr<IAudioOutput> makeOfflineOutput(RingBuffer& sink, double speedMultiple) {
    return std::make_unique<OfflineOutput>(sink, speedMultiple);
}

std::vector<float> capturedAudio(const IAudioOutput& output) {
    const auto* offline = dynamic_cast<const OfflineOutput*>(&output);
    return offline ? offline->captured() : std::vector<float>{};
}

}  // namespace xpcog
