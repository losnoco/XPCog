#include "xpcog/core/audio/Fader.hpp"

#include "xpcog/core/AudioFormat.hpp"

#include <algorithm>
#include <cstddef>

namespace xpcog {

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

    // Unroll the ring into chronological order. The oldest frame sits at the
    // write cursor once the ring has wrapped, at zero before then.
    const auto        channels = static_cast<std::size_t>(channels_);
    const std::size_t capacity = history_.size() / channels;
    tailFrames_                = std::min(historyFilled_, capacity);
    if (tailFrames_ > 0) {
        const std::size_t start = (historyFilled_ < capacity) ? 0 : historyWrite_;
        for (std::size_t frame = 0; frame < tailFrames_; ++frame) {
            const std::size_t source = (start + frame) % capacity;
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
    level_              = 0.0;
    target_             = 1.0;
    const double frames = static_cast<double>(fadeFrames());
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
            // Equal-gain crossfade: the outgoing tail decays with the complement
            // of the incoming ramp. Linear complements suit correlated material
            // -- the same track either side of a short seek -- which is the case
            // a transport fade serves; they sum to unity, so there is no dip.
            const auto in  = static_cast<float>(level_);
            const auto out = static_cast<float>(1.0 - level_);

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
