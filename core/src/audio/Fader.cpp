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
        // signal at whatever partial gain it had reached.
        level_  = 1.0;
        target_ = 1.0;
        step_   = 0.0;
    }
}

void Fader::setFadeMilliseconds(double milliseconds) {
    milliseconds_ = std::max(milliseconds, 0.0);
}

void Fader::prepare(const AudioFormat& format) {
    sampleRate_ = format.sampleRate;
    channels_   = static_cast<int>(format.channels);
    reset();
}

void Fader::beginFadeIn() {
    level_  = 0.0;
    target_ = 1.0;

    // Frames, not samples: the ramp is a property of time, so it must not depend
    // on the channel count.
    const double frames = sampleRate_ * milliseconds_ / 1000.0;
    step_               = (frames > 0.0) ? (1.0 / frames) : 1.0;
}

void Fader::reset() {
    if (!enabled_ || sampleRate_ <= 0.0) {
        level_  = 1.0;
        target_ = 1.0;
        step_   = 0.0;
        return;
    }
    beginFadeIn();
}

bool Fader::active() const {
    // Unity with nothing pending is not just inactive but *bit*-transparent,
    // because the chain skips it rather than multiplying by one.
    return enabled_ && (level_ != target_ || target_ != 1.0);
}

void Fader::process(float* samples, std::size_t frames) {
    if (frames == 0 || channels_ <= 0 || !active()) {
        return;
    }

    const auto channels = static_cast<std::size_t>(channels_);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (level_ != target_) {
            level_ = (step_ >= 0.0) ? std::min(level_ + step_, target_)
                                    : std::max(level_ + step_, target_);
        }

        // Once landed the rest of the block is untouched, which keeps the common
        // case -- a ramp that finishes early in a block -- from paying for a
        // multiply on every remaining sample.
        if (level_ == 1.0 && target_ == 1.0) {
            return;
        }

        const auto gain = static_cast<float>(level_);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            samples[(frame * channels) + channel] *= gain;
        }
    }
}

}  // namespace xpcog
