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
#include <string_view>
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
        /// Ask for the device exclusively. A backend that cannot do it, or a
        /// device already held by something else, falls back to sharing -- so
        /// this is a preference rather than a demand, and exclusiveHeld() says
        /// which way it went.
        bool exclusive = false;
        /// 0 lets the backend choose. Smaller means lower latency and more
        /// callbacks; the feeder must keep up either way.
        std::uint32_t bufferFrames = 0;
        /// What the device should carry. Float unless something needs the bits
        /// to arrive exactly as written, and the thing that needs that is DoP:
        /// DSD travels inside PCM as a 0x05/0xFA marker plus payload, and a
        /// float path that scales or dithers turns it into noise at the DAC.
        ///
        /// A request, like `exclusive`. A backend that cannot open the device
        /// this way opens it in float instead and says so through
        /// negotiatedFormat(), because refusing to play is a worse answer to "I
        /// would rather not be converted" than converting.
        ///
        /// S16, S24, S32 and F32 only -- the layouts a device is opened in. The
        /// rest of SampleFormat describes what a *decoder* produces.
        SampleFormat format = SampleFormat::F32;
    };

    virtual ~IAudioOutput() = default;

    virtual bool start(const Config& config) = 0;
    virtual void stop()                      = 0;
    virtual void pause()                     = 0;
    virtual void resume()                    = 0;

    /// Whether pause() should hand the device back to the system, or hold it and
    /// emit silence. Cog's `suspendOutputOnPause`, which it reads in
    /// OutputCoreAudio (:127, :1091).
    ///
    /// Suspending is the polite default and is what this did unconditionally
    /// before the setting existed: a paused player has no business holding a
    /// device another application wants, and on an exclusive-mode device it has
    /// no business holding the *only* one. Holding it is what someone chooses
    /// when reacquiring costs more than it saves -- an exclusive device another
    /// application may take in the gap, or a DAC that clicks or drops its lock
    /// each time the stream stops.
    ///
    /// A no-op for a backend with nothing to release, and for OfflineOutput,
    /// which has no device at all.
    virtual void setSuspendOnPause(bool suspend) { static_cast<void>(suspend); }

    /// What the device actually negotiated, which may differ from the request.
    [[nodiscard]] virtual AudioFormat negotiatedFormat() const = 0;

    [[nodiscard]] virtual double latencySeconds() const = 0;

    /// Whether this output can be asked to run at `sampleRate`.
    ///
    /// The default is the range every current device backend accepts, and it is
    /// stated here rather than in the engine because it is a property of the
    /// backend: miniaudio documents 8,000 to 384,000 (miniaudio.h:126), and the
    /// native outputs that will replace it may answer differently -- a DAC fed
    /// DoP runs at 352,800 or 705,600, which is the whole reason this question
    /// is asked out loud.
    ///
    /// It is asked because DSD does not arrive at a rate a device will run:
    /// 352,800 or 705,600 Hz, one byte per channel per frame. Cog never asks
    /// either -- its output keeps the device's own format and resamples
    /// everything into it (OutputCoreAudio.m, -outputFormatForInputFormat:),
    /// raising the rate only for a DoP carrier the device has confirmed.
    [[nodiscard]] virtual bool supportsSampleRate(double sampleRate) const {
        return sampleRate >= 8000.0 && sampleRate <= 384000.0;
    }

    /// The rate this output would rather run at, or 0 when it cannot say. Used
    /// when the track's own rate is one supportsSampleRate() has refused.
    ///
    /// `deviceId` is the device that is *about to be opened*, empty for the
    /// system default. It is a parameter rather than remembered state because
    /// this is asked before start(), so there is nothing to remember yet -- and
    /// answering for the default device while another one is about to be opened
    /// is how a DSD track ends up resampled to the wrong rate on the way to a
    /// DAC that would have taken it.
    [[nodiscard]] virtual double preferredSampleRate(std::string_view deviceId = {}) const {
        static_cast<void>(deviceId);
        return 0.0;
    }

    /// The rate this output would **really** run at if asked for `wanted`.
    ///
    /// Not the same question as supportsSampleRate(), and the difference is the
    /// whole point. That one asks whether the rate can be *requested*; this one
    /// asks what the hardware will be doing afterwards. A backend is entitled to
    /// accept a request it has no intention of honouring and quietly convert,
    /// which is exactly what miniaudio does: `ma_device_init()` keeps the
    /// requested rate in `device.sampleRate`, runs the hardware at
    /// `internalSampleRate`, and inserts a **linear** resampler between them
    /// (see `tools/ma-rate-probe`). Nothing downstream can see that, because
    /// negotiatedFormat() reports the requested rate -- so AudioConverter is
    /// told the rates match, correctly declines to resample, and soxr sits
    /// unused while a linear resampler does the work.
    ///
    /// Asking here, before the converter is built, is what lets the engine open
    /// the device at a rate it will actually run and do the conversion itself.
    ///
    /// `exclusive` matters and is not a hint: an exclusive stream owns the
    /// hardware and switches it to whatever is asked for, so the honest answer
    /// there is usually `wanted` itself. A shared stream never switches the
    /// device, so everything that is not its current rate gets converted.
    /// Answering the same way for both would either give up bit-perfect
    /// exclusive playback or keep the silent resampling in shared.
    ///
    /// The default returns `wanted`, which is the truth for an output that
    /// really does run at whatever it is handed -- OfflineOutput, and the test
    /// doubles.
    [[nodiscard]] virtual double effectiveSampleRate(double            wanted,
                                                     std::string_view  deviceId = {},
                                                     bool exclusive = false) const {
        static_cast<void>(deviceId);
        static_cast<void>(exclusive);
        return wanted;
    }

    /// Whether the running device is actually held exclusively. False when it
    /// was not asked for, when the backend cannot, and when the device refused.
    [[nodiscard]] virtual bool exclusiveHeld() const { return false; }

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

/// The playback devices, without opening one or owning an output.
///
/// For the preferences pane, which needs the list to draw a picker and has no
/// business holding the thing that plays audio. Enumerating means building the
/// backend's context, so this is a call to make when a dialog opens rather than
/// one to poll.
[[nodiscard]] std::vector<DeviceInfo> enumerateOutputDevices();

/// Which of `devices` a stored choice means, or empty for the system default.
///
/// By id first and then by name, which is Cog's rule
/// (Preferences/OutputsArrayController.m) and is not redundancy: a device that
/// has been unplugged and put back, or a machine that has rebooted, can present
/// the same hardware under a different id. Matching only on id would silently
/// drop the listener back to the default speakers; the name is what they know
/// it by.
///
/// A choice that matches nothing returns empty -- the default device, for now.
/// The stored setting is deliberately left alone, so plugging the device back
/// in restores it rather than requiring it to be picked again.
[[nodiscard]] std::string resolveOutputDevice(const std::vector<DeviceInfo>& devices,
                                              std::string_view wantedId,
                                              std::string_view wantedName);

}  // namespace xpcog
