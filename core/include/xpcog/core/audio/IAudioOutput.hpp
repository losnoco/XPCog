// The audio output seam, replacing Cog's OutputCoreAudio (AUHAL + raw CoreAudio
// HAL, Audio/Output/OutputCoreAudio.m, 1432 lines of Apple-only code).
//
// Everything behind this interface is platform-specific; nothing in front of it
// is. Keeping device enumeration and hot-plug here is also what makes it possible
// to swap in native WASAPI/CoreAudio backends later without touching the engine.

#pragma once

#include "xpcog/core/AudioFormat.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xpcog {

class RingBuffer;

struct DeviceInfo {
    std::string id;    ///< opaque, stable across runs where the backend allows
    std::string name;  ///< human-readable
    bool        isDefault = false;
};

class IAudioOutput {
public:
    struct Config {
        double        sampleRate = 44100.0;
        std::uint32_t channels   = 2;
        /// Empty selects the system default device.
        std::string deviceId;
        /// 0 lets the backend choose. Smaller means lower latency and more
        /// callbacks; the feeder must keep up either way.
        std::uint32_t bufferFrames = 0;
    };

    virtual ~IAudioOutput() = default;

    virtual bool start(const Config& config) = 0;
    virtual void stop()                      = 0;
    virtual void pause()                     = 0;
    virtual void resume()                    = 0;

    /// What the device actually negotiated, which may differ from the request.
    [[nodiscard]] virtual AudioFormat negotiatedFormat() const = 0;

    [[nodiscard]] virtual double latencySeconds() const = 0;

    [[nodiscard]] virtual std::vector<DeviceInfo> devices() const = 0;

    /// Linear gain applied in the callback. Read atomically; safe to call from
    /// any thread at any time.
    virtual void  setVolume(float gain) = 0;
    [[nodiscard]] virtual float volume() const = 0;

    /// Ramps a second gain, independent of setVolume(), to `target` over
    /// `milliseconds`. Safe from any thread.
    ///
    /// This is where the pause and stop fades have to happen, and the reason is
    /// buffering. Those fades apply to audio that is *already queued* for the
    /// device, so a stage upstream of the ring cannot reach it -- fading there
    /// would take effect only once the queue had drained, by which time the
    /// transport has long since stopped. The seek fade has no such problem: it
    /// discards the queue, so the DSP chain's crossfade is heard immediately.
    ///
    /// Separate from setVolume() so a fade neither reads nor overwrites the
    /// user's volume; the callback multiplies by both.
    virtual void rampGain(float target, double milliseconds) = 0;

    /// True while a rampGain() is still in progress, so a caller can wait for a
    /// fade out to be audible before stopping the device.
    [[nodiscard]] virtual bool ramping() const = 0;

    /// Number of times the callback found the ring empty. A rising count means
    /// the feeder is not keeping up.
    [[nodiscard]] virtual std::uint64_t underrunCount() const = 0;

    /// Frames handed to the device since start(). This is the playback clock:
    /// it is what track-change notifications are timed against, so a seam is
    /// announced when it becomes audible rather than when it was decoded.
    /// Counts frames requested by the callback, including any silenced tail.
    [[nodiscard]] virtual std::uint64_t framesPlayed() const = 0;

    /// Invoked from a NON-real-time thread when the device disappears or changes.
    /// Replaces Cog's AudioObjectAddPropertyListener on the CoreAudio HAL.
    virtual void setDeviceInvalidatedCallback(std::function<void()> callback) = 0;

    /// Where to publish the audio being played, for a visualiser. Null to stop.
    ///
    /// Here rather than upstream because this is the last point the audio exists
    /// before the driver takes it -- post-DSP, post-volume, post-fade -- so what a
    /// visualiser sees is what is about to be heard. Cog taps further up
    /// (`-postVisPCM:`) and then has to read behind its own write cursor by the
    /// device latency to undo the difference; tapping here means there is no
    /// difference to undo. See AudioTap.
    ///
    /// May be called while playing: implementations hold it atomically, because the
    /// callback reads it.
    virtual void setTap(class AudioTap* tap) = 0;
};

/// Reads interleaved float32 from `sink`. The caller owns the ring and must keep
/// it alive for the lifetime of the returned output.
[[nodiscard]] std::unique_ptr<IAudioOutput> makeMiniaudioOutput(RingBuffer& sink);

}  // namespace xpcog
