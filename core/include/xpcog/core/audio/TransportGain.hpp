// The output stage's volume and transport fade, in one place.
//
// This existed twice -- once in MiniaudioOutput's callback and once in
// OfflineOutput -- and the two disagreed, which is the entire reason it is now a
// class. The real device's copy skipped its work whenever the ramp had *settled*:
//
//     if (fade != target || gain != 1.0F) { ...apply... }
//
// Settled at one is indeed nothing to do. Settled at *zero* is silence, and
// skipping the multiply played the buffer at full volume instead. So a faded pause
// (which leaves both at zero) was followed, on resume, by a burst at full volume
// until rampGain made the two differ again -- at which point the gain ramped up
// from zero, having already been heard. Reported as "it plays at full volume then
// fades in from silence", which is precisely what those two lines do.
//
// The offline copy had the condition right, requiring all three to be unity. That
// is why no test caught it: the test double was correct and the device was not.
// Two implementations of one rule is the defect; the wrong condition was only how
// it showed up.
//
// Header-only, and deliberately. apply() runs in the device callback, where a
// cross-translation-unit call in the inner loop is a cost with nothing to show for
// it.
//
// Real-time safety: apply() allocates nothing, locks nothing and calls nothing.
// `level_` is atomic because the ramp is written by the callback and read by
// whichever thread is waiting for it to land -- a plain float there was a data
// race that stop() depended on. It is loaded and stored once per buffer, not per
// frame, so the ramp itself still runs on a local.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>

namespace xpcog {

class TransportGain {
public:
    /// Back to unity, with no ramp outstanding.
    ///
    /// For a device that is not running: this moves the value the callback owns.
    /// It exists because the ramp outlives the device otherwise -- a faded stop
    /// leaves the level at zero, and starting a new track without clearing it
    /// played the whole track silently.
    void reset() noexcept {
        level_.store(1.0F, std::memory_order_relaxed);
        target_.store(1.0F, std::memory_order_relaxed);
        step_.store(1.0F, std::memory_order_relaxed);
    }

    /// Ramps towards `target` over `milliseconds`. Any thread.
    ///
    /// A zero length, or no sample rate yet, means snap -- so a caller waiting for
    /// ramping() to clear cannot hang on a ramp that could never advance.
    void rampTo(float target, double milliseconds, double sampleRate) noexcept {
        const double frames = sampleRate * milliseconds / 1000.0;
        // Step first, target second: the callback may run between these two, and
        // seeing a new step with the old target is harmless where seeing a new
        // target with a stale step would ramp at the wrong speed.
        step_.store(frames > 0.0 ? static_cast<float>(1.0 / frames) : 1.0F,
                    std::memory_order_relaxed);
        target_.store(target, std::memory_order_relaxed);
    }

    [[nodiscard]] bool ramping() const noexcept {
        return level_.load(std::memory_order_relaxed) !=
               target_.load(std::memory_order_relaxed);
    }

    /// The level the callback has actually reached, for tests and diagnostics.
    [[nodiscard]] float level() const noexcept {
        return level_.load(std::memory_order_relaxed);
    }

    /// Multiplies `count` interleaved samples in place by `volume` times the fade,
    /// advancing the fade one step per *frame*.
    ///
    /// Per frame rather than per sample: a ramp stepped per sample would move at
    /// the channel count's speed, and the channels would be at different gains
    /// within one frame -- which skews the stereo image while it runs.
    void apply(float* samples, std::size_t count, std::size_t channels,
               float volume) noexcept {
        const float target = target_.load(std::memory_order_relaxed);
        const float step   = step_.load(std::memory_order_relaxed);
        float       level  = level_.load(std::memory_order_relaxed);

        // Skipped only when the product is exactly one. Note what is *not* tested
        // here: whether the ramp has settled. See the file comment.
        if (level == 1.0F && target == 1.0F && volume == 1.0F) {
            return;
        }

        const std::size_t stride = std::max<std::size_t>(channels, 1);
        for (std::size_t frame = 0; frame * stride < count; ++frame) {
            if (level != target) {
                level = (target > level) ? std::min(level + step, target)
                                         : std::max(level - step, target);
            }
            const float combined = volume * level;
            for (std::size_t channel = 0; channel < stride; ++channel) {
                const std::size_t index = (frame * stride) + channel;
                if (index < count) {
                    samples[index] *= combined;
                }
            }
        }

        level_.store(level, std::memory_order_relaxed);
    }

private:
    /// Written by the callback, read by anyone waiting on the ramp.
    std::atomic<float> level_{1.0F};
    std::atomic<float> target_{1.0F};
    std::atomic<float> step_{1.0F};
};

}  // namespace xpcog
