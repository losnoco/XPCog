#include "xpcog/core/audio/LevelMeter.hpp"

#include <algorithm>
#include <cmath>

namespace xpcog {

namespace {

/// The longest stretch one update looks at. A slow frame reads this much rather
/// than everything since the last.
constexpr std::size_t kMaxWindow = 16384;

[[nodiscard]] double decibels(double linear) {
    if (linear <= 0.0) {
        return LevelMeter::kFloor;
    }
    return std::max(LevelMeter::kFloor, 20.0 * std::log10(linear));
}

}  // namespace

void LevelMeter::fall(Channel& channel, double& held, double peak, double rms,
                      double elapsedSeconds) {
    const double drop = kFallDecibelsPerSecond * elapsedSeconds;
    channel.peak      = std::max(peak, channel.peak - drop);
    channel.rms       = std::max(rms, channel.rms - drop);

    held += elapsedSeconds;
    if (peak >= channel.hold) {
        channel.hold = peak;
        held         = 0.0;
    } else if (held > kHoldSeconds) {
        channel.hold = std::max(channel.peak, channel.hold - drop);
    }
}

void LevelMeter::measure(std::size_t channel, const float* samples, std::size_t count,
                         double elapsedSeconds) {
    if (channel >= channels_.size()) {
        return;
    }

    double peak   = 0.0;
    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double sample = samples[i];
        peak                = std::max(peak, std::abs(sample));
        energy += sample * sample;
    }
    const double rms = (count > 0) ? std::sqrt(energy / static_cast<double>(count)) : 0.0;

    if (peak >= 1.0) {
        channels_[channel].clipped = true;
    }
    fall(channels_[channel], heldFor_[channel], decibels(peak), decibels(rms), elapsedSeconds);
}

void LevelMeter::update(const AudioTap& tap, double elapsedSeconds, double sampleRate,
                        bool playing) {
    const auto wanted = static_cast<std::size_t>(std::max(0.0, elapsedSeconds * sampleRate));
    const std::size_t window = std::clamp<std::size_t>(wanted, 1, kMaxWindow);
    scratch_.resize(window);

    constexpr std::array<TapLane, 2> lanes = {TapLane::Left, TapLane::Right};
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        if (playing && tap.readLatest(scratch_.data(), window, lanes[i])) {
            measure(i, scratch_.data(), window, elapsedSeconds);
        } else {
            measure(i, nullptr, 0, elapsedSeconds);
        }
    }
}

void LevelMeter::clearClip() noexcept {
    for (Channel& channel : channels_) {
        channel.clipped = false;
    }
}

}  // namespace xpcog
