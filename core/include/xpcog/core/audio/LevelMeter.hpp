// Peak and RMS levels of what is being heard, for the VU meter and the Signal
// Inspector. Port of Cog's SignalMetricsModel (SignalInspector/SignalMeters.swift).
//
// Reads AudioTap on the interface thread rather than metering in the output
// callback as Cog's engine does: the tap already has the samples, so the
// real-time path gains nothing. The cost of that is that the tap keeps the first
// two channels and no more, so surround is metered as its front pair. Recorded
// in docs/PORTING.md.

#pragma once

#include "xpcog/core/audio/AudioTap.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace xpcog {

class LevelMeter {
public:
    /// Where the meters bottom out, in dBFS.
    static constexpr double kFloor = -60.0;
    /// How long a peak is held, and how fast the meters fall after it.
    static constexpr double kHoldSeconds         = 1.5;
    static constexpr double kFallDecibelsPerSecond = 24.0;

    struct Channel {
        double peak = kFloor;  ///< dBFS, from kFloor up
        double rms  = kFloor;
        double hold = kFloor;  ///< the highest peak lately
        /// A peak reached full scale since clearClip().
        bool clipped = false;
    };

    /// Advances by `elapsedSeconds`, measuring the newest samples that cover
    /// it. At `sampleRate`, the rate the tap is being written at. With nothing
    /// to read, or `playing` false, the meters fall to rest.
    void update(const AudioTap& tap, double elapsedSeconds, double sampleRate, bool playing);

    /// The same from samples in hand, for a caller with its own source.
    void measure(std::size_t channel, const float* samples, std::size_t count,
                 double elapsedSeconds);

    [[nodiscard]] const Channel& left() const noexcept { return channels_[0]; }
    [[nodiscard]] const Channel& right() const noexcept { return channels_[1]; }

    void clearClip() noexcept;

private:
    void fall(Channel& channel, double& held, double peak, double rms, double elapsedSeconds);

    std::array<Channel, 2> channels_{};
    std::array<double, 2>  heldFor_{};
    std::vector<float>     scratch_;
};

}  // namespace xpcog
