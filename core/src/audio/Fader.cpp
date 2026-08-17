#include "xpcog/core/audio/Fader.hpp"

#include "xpcog/core/AudioFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace xpcog {
namespace {

/// Spelled out rather than reaching for M_PI, which MSVC does not define.
constexpr double xpcogPi = std::numbers::pi;

}  // namespace

void Fader::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        // Turned off mid-ramp, so land immediately rather than freezing the
        // signal at whatever partial gain it had reached. The history goes too:
        // stale audio must not fade out over a seek made minutes later.
        level_         = 1.0;
        target_        = 1.0;
        step_          = 0.0;
        crossfading_   = false;
        tailFrames_    = 0;
        historyFilled_ = 0;
        historyWrite_  = 0;
    }
}

void Fader::noteDiscardedFrames(std::size_t frames) {
    pendingDiscarded_.store(frames, std::memory_order_release);
}

void Fader::setFadeMilliseconds(double milliseconds) {
    milliseconds_ = std::max(milliseconds, 0.0);
    rebuildHistory();
}

std::size_t Fader::fadeFrames() const {
    return static_cast<std::size_t>(sampleRate_ * milliseconds_ / 1000.0);
}

void Fader::rebuildHistory() {
    const std::size_t frames   = fadeFrames();
    const auto        channels = static_cast<std::size_t>(std::max(channels_, 0));
    history_.assign(frames * channels, 0.0F);
    tail_.assign(frames * channels, 0.0F);
    historyWrite_  = 0;
    historyFilled_ = 0;
    tailFrames_    = 0;
    crossfading_   = false;
}

void Fader::prepare(const AudioFormat& format) {
    sampleRate_ = format.sampleRate;
    channels_   = static_cast<int>(format.channels);
    rebuildHistory();
    reset();
}

void Fader::reset() {
    if (!enabled_ || sampleRate_ <= 0.0 || channels_ <= 0) {
        level_       = 1.0;
        target_      = 1.0;
        step_        = 0.0;
        crossfading_ = false;
        tailFrames_  = 0;
        return;
    }

    // The tail is the *newest* frames of the history, as many as the flush is
    // discarding. Taking the oldest instead replays audio already heard, which is
    // a stutter rather than a fade -- the ring is normally near full, so that
    // mistake is small enough to survive a test and still be audible.
    const auto        channels = static_cast<std::size_t>(channels_);
    const std::size_t capacity = history_.size() / channels;
    const std::size_t available = std::min(historyFilled_, capacity);
    tailFrames_ = std::min(pendingDiscarded_.exchange(0, std::memory_order_acquire),
                           available);

    if (tailFrames_ > 0) {
        // historyWrite_ is one past the newest frame, so back up by the tail
        // length to find where the discarded audio begins.
        const std::size_t begin =
            (historyWrite_ + capacity - tailFrames_) % capacity;
        for (std::size_t frame = 0; frame < tailFrames_; ++frame) {
            const std::size_t source = (begin + frame) % capacity;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                tail_[(frame * channels) + channel] =
                    history_[(source * channels) + channel];
            }
        }
    }
    tailRead_    = 0;
    crossfading_ = tailFrames_ > 0;

    // The incoming side ramps up regardless; with no tail this is the plain
    // track-start fade in. Frames, not samples: the ramp is a property of time,
    // so it must not depend on the channel count.
    //
    // When crossfading, the ramp spans the tail rather than the nominal fade
    // time, so the two sides finish together. Otherwise a tail shorter than the
    // fade ends abruptly at whatever level it had reached -- a step, which is the
    // artefact the fade exists to remove.
    level_  = 0.0;
    target_ = 1.0;
    const double frames = static_cast<double>(crossfading_ ? tailFrames_ : fadeFrames());
    step_               = (frames > 0.0) ? (1.0 / frames) : 1.0;

    // The old history described the old position; a second seek during the
    // crossfade must not be fed audio from two positions ago.
    historyFilled_ = 0;
    historyWrite_  = 0;
}

bool Fader::active() const {
    // Enabled means always run: the rolling history has to see every block, or
    // the tail available at the next seek would have holes where the chain
    // skipped this stage. process() is still copy-only once the ramp has landed
    // and no tail is playing.
    return enabled_;
}

void Fader::captureFrame(const float* frame) {
    const auto        channels = static_cast<std::size_t>(channels_);
    const std::size_t capacity = history_.size() / channels;
    if (capacity == 0) {
        return;
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
        history_[(historyWrite_ * channels) + channel] = frame[channel];
    }
    historyWrite_  = (historyWrite_ + 1) % capacity;
    historyFilled_ = std::min(historyFilled_ + 1, capacity);
}

void Fader::process(float* samples, std::size_t frames) {
    if (frames == 0 || channels_ <= 0 || !enabled_) {
        return;
    }

    const auto channels = static_cast<std::size_t>(channels_);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        float* current = samples + (frame * channels);

        if (level_ != target_) {
            level_ = (step_ >= 0.0) ? std::min(level_ + step_, target_)
                                    : std::max(level_ + step_, target_);
        }

        if (level_ != 1.0 || crossfading_) {
            // Constant power, not constant amplitude, and this is a deliberate
            // departure from Cog's linear ramp.
            //
            // A seek lands somewhere unrelated, so the two sides of the crossfade
            // are uncorrelated: their powers add, and linear complements of 0.5
            // each give 0.5 of the power -- an audible 3 dB dip in the middle of
            // every seek. Sine and cosine legs keep the sum of squares at one,
            // which is the right curve for uncorrelated material. (For a plain
            // ramp in, with no tail, the curve only shapes silence-to-signal and
            // either choice sounds like a fade.)
            const double theta = level_ * (xpcogPi / 2.0);
            const auto   in    = static_cast<float>(std::sin(theta));
            const auto   out   = static_cast<float>(std::cos(theta));

            for (std::size_t channel = 0; channel < channels; ++channel) {
                float mixed = current[channel] * in;
                if (crossfading_ && tailRead_ < tailFrames_) {
                    mixed += tail_[(tailRead_ * channels) + channel] * out;
                }
                current[channel] = mixed;
            }
            if (crossfading_) {
                ++tailRead_;
                if (tailRead_ >= tailFrames_ || level_ >= 1.0) {
                    crossfading_ = false;
                }
            }
        }

        // After the mix, so the history records what was actually emitted --
        // that is what will be audible right before any future seek, and
        // therefore what its fade out must start from.
        captureFrame(current);
    }
}

}  // namespace xpcog
