// The OS's idea of what is playing, and its transport controls.
//
// On macOS that is MediaPlayer.framework: MPNowPlayingInfoCenter fills in the
// Control Centre / Now Playing widget and the lock screen, and
// MPRemoteCommandCenter is how a Mac application receives the media keys at all
// -- the old approach of tapping the HID event stream stopped being viable, and
// Cog carries a whole CogRemoteControl subproject for what is now a dozen
// blocks. Registering the commands is what makes the keys arrive.
//
// The two are one class rather than two because on every platform they are one
// object: you cannot claim the keys without also saying what is playing, and a
// now-playing entry with no working controls is worse than none.
//
// All three platforms are implemented: MediaPlayer.framework on macOS, SMTC on
// Windows, MPRIS on Linux. Anything else gets the base class, which does nothing
// -- so the window wires up identically everywhere and gaining a platform is one
// subclass, not a new call site.
//
// The interface is the union of what the three can do, and the parts only one of
// them supports are the ones to be careful with. MPRIS is much the richest: it
// is a general remote-control protocol, so it asks to raise the window, to quit
// the application and to set the volume, none of which SMTC or macOS can
// express. Those arrive as signals like any other command, and the two
// implementations that cannot produce them simply never publish them.
//
// --- Threading -------------------------------------------------------------
//
// Every command below originates on a thread that is not the user interface's:
// SMTC calls back on a WinRT thread, MPRIS on whichever thread pumps the D-Bus
// connection, MediaPlayer.framework on a dispatch queue. Qt hid that behind
// queued connections, which was convenient and invisible; xpcog::Signal is
// synchronous and hides nothing.
//
// So the hop is explicit and it lives here rather than in every subscriber. An
// implementation calls publishOnUiThread(), which routes through the Dispatcher
// this object was built with, and a subscriber may touch its widgets without
// checking which thread it is on. Subclasses that publish directly are a bug of
// exactly the kind Qt used to absorb.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::platform {

struct NowPlayingInfo {
    std::string title;
    std::string artist;
    std::string album;
    /// Seconds. Zero means unknown, which is how a stream reports itself.
    double duration = 0.0;
    double position = 0.0;

    /// The cover art exactly as the file carried it -- JPEG or PNG bytes, not a
    /// decoded image. Empty when the track has none, and the OS falls back to
    /// the application icon.
    ///
    /// Encoded rather than decoded because every one of the three backends wants
    /// bytes in the end: Windows wraps them in an IStream, macOS in an NSData,
    /// MPRIS writes them to a file it can name in a URL. Handing them a decoded
    /// image meant each one re-encoded it, and Library::artwork() had the
    /// original bytes all along.
    std::vector<std::byte> artwork;
};

/// Runs a callable on the thread that owns the user interface.
///
/// Supplied by the application, because this layer deliberately does not know
/// what the interface is built with. In XPCog it wraps wxEvtHandler::CallAfter.
using Dispatcher = std::function<void(std::function<void()>)>;

class MediaIntegration {
public:
    /// The implementation for this platform, or a do-nothing one where there is
    /// none. Never null, so callers have no branch to forget.
    ///
    /// `dispatch` must remain safe to call from any thread for as long as the
    /// returned object lives.
    ///
    /// `nativeWindow` is a top-level window's native handle -- an HWND on
    /// Windows, ignored everywhere else. It is a parameter rather than something
    /// discovered because Windows genuinely needs one: SMTC binds to an HWND
    /// through ISystemMediaTransportControlsInterop::GetForWindow, since the
    /// GetForCurrentView() route needs a CoreWindow that only UWP has. The
    /// version of this file that came before went looking for a window instead,
    /// found none -- it was constructed before the window existed -- and carried
    /// a deferred-acquisition retry for the rest of its life. Constructing this
    /// after the window and passing the handle deletes all of that.
    [[nodiscard]] static std::unique_ptr<MediaIntegration> create(Dispatcher dispatch,
                                                                 void* nativeWindow);

    explicit MediaIntegration(Dispatcher dispatch) : dispatch_(std::move(dispatch)) {}

    MediaIntegration(const MediaIntegration&)            = delete;
    MediaIntegration& operator=(const MediaIntegration&) = delete;

    virtual ~MediaIntegration() = default;

    /// What is playing. Called on a track change, not per tick.
    virtual void setNowPlaying(const NowPlayingInfo& info) { (void)info; }

    /// Playing, paused or stopped, plus where the playhead is.
    ///
    /// Separate from setNowPlaying because the OS wants the elapsed time and
    /// the rate to move together: given both it extrapolates the position
    /// itself, so this is cheap to call on the transport's tick and the Now
    /// Playing widget still counts smoothly between calls.
    virtual void setPlaybackState(bool playing, bool paused, double position) {
        (void)playing;
        (void)paused;
        (void)position;
    }

    /// Nothing is playing. Removes the entry rather than leaving a stale one.
    virtual void clear() {}

    /// The output gain, 0..1.
    ///
    /// Only MPRIS uses this: it publishes the volume as a property a desktop
    /// environment both reads and writes, so the value has to be pushed here or
    /// the panel's slider sits at whatever it last set while the audio does
    /// something else. SMTC and macOS have no equivalent and ignore it.
    virtual void setVolume(float gain) { (void)gain; }

    // --- Commands from the OS, all delivered on the UI thread ---------------

    Signal<>       playRequested;
    Signal<>       pauseRequested;
    Signal<>       playPauseRequested;
    Signal<>       stopRequested;
    Signal<>       nextRequested;
    Signal<>       previousRequested;
    Signal<double> seekRequested;

    /// Bring the window forward. MPRIS only -- it is how clicking a desktop
    /// panel's media widget is meant to reach the player.
    Signal<> raiseRequested;
    /// Quit the application. MPRIS only.
    Signal<> quitRequested;
    /// Set the output gain, 0..1. MPRIS only.
    Signal<float> volumeRequested;
    /// Add and play this URL. MPRIS only -- it is the OpenUri method, which the
    /// protocol pairs with the list of schemes the player claims to support, so
    /// this exists rather than that list being a promise nothing keeps.
    Signal<Url> openUrlRequested;

protected:
    /// Publishes `signal` on the user interface's thread.
    ///
    /// The only way a subclass should ever reach a signal above. Arguments are
    /// copied into the callable, because the OS callback that produced them
    /// returns long before this runs.
    template <typename... Args>
    void publishOnUiThread(Signal<Args...>& signal, Args... args) const {
        // `this` is captured because the signal is a member; the dispatcher is
        // documented to run nothing after the object is destroyed, which is what
        // makes that safe. The alternative -- capturing a shared_ptr -- would put
        // a lifetime knot in the one layer that must stay legible.
        dispatch_([&signal, args...]() { signal.publish(args...); });
    }

private:
    Dispatcher dispatch_;
};

}  // namespace xpcog::platform
