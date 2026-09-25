// The path to the system's spatializer, for a stream wider than its device.
//
// Private to MiniaudioOutput, which owns the ring, the tap, the gain and the
// clock, and lends all four to this through a Source -- so the stream reads the
// same ring and publishes to the same tap as the device path does.
//
// Real only on macOS (SpatialStreamMac.mm). There a HAL client is given the
// device's own channel count, so 7.1 for AirPods is folded to stereo before the
// system sees it, and the system spatializes only what reaches it through
// AVFoundation. Everywhere else the OS spatializer presents itself as a
// surround endpoint and miniaudio already hands it the full width, so the
// stand-in (SpatialStreamNone.cpp) declines every stream.
//
// What makes it different from a device is depth. The renderer wants about a
// second queued ahead of its clock and starves on less -- with 100 ms it played
// in pulses -- so nothing that must be *heard* at a moment can be applied as
// audio is pulled. Gain is set on the renderer, the tap is fed as the clock
// passes each buffer, and the clock is the renderer's own.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xpcog {
namespace detail {

/// What the stream borrows from its output. Every member is called on the
/// stream's own queue, never on a real-time thread.
struct SpatialSource {
    /// Up to `frames` whole frames of what is in the ring, raw -- no gain, no
    /// padding. Returns how many it got. `flushed` is set when the engine has
    /// asked for the ring to be dropped, which is a seek.
    std::function<std::size_t(float* out, std::size_t frames, bool& flushed)> pull;
    /// `frames` frames have just become audible. For the visualiser tap.
    std::function<void(const float* samples, std::size_t frames)> heard;
    /// The gain to play at now, `elapsedFrames` of wall-clock time after the
    /// last time this was asked -- volume times the transport fade.
    std::function<float(std::size_t elapsedFrames)> gain;
    /// The renderer has failed. Fired once.
    std::function<void()> failed;
};

class SpatialStream {
public:
    virtual ~SpatialStream() = default;

    /// Stops the clock where it is. What is queued stays queued, unfaded, and is
    /// what plays on resume -- the fade out was applied at the renderer.
    virtual void pause() = 0;
    virtual void resume() = 0;

    /// Frames heard since the stream opened. Stands still while paused, while
    /// priming and across an underrun, and never counts a frame a flush threw
    /// away. Any thread.
    [[nodiscard]] virtual std::uint64_t framesPlayed() const = 0;

    /// How much is queued ahead of the ear.
    [[nodiscard]] virtual double latencySeconds() const = 0;
};

/// Whether `channels` of audio bound for `deviceUid` (empty for the system
/// default) should go this way: the device has fewer channels than the stream,
/// and this platform has a spatializer to hand it to.
[[nodiscard]] bool spatialStreamWanted(std::uint32_t channels, const std::string& deviceUid);

/// Null when the stream could not be built, in which case the caller opens its
/// ordinary device instead.
[[nodiscard]] std::unique_ptr<SpatialStream>
openSpatialStream(double sampleRate, std::uint32_t channels, std::uint32_t channelConfig,
                  const std::string& deviceUid, SpatialSource source);

}  // namespace detail
}  // namespace xpcog
