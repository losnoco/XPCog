// Windows media keys and the Now Playing overlay, via the System Media
// Transport Controls.
//
// SMTC is the Windows counterpart to the macOS MPNowPlayingInfoCenter /
// MPRemoteCommandCenter pair, and it works the same way round: registering the
// controls is what makes the media keys arrive at all, and the metadata and the
// keys come from one object rather than two.
//
// Three things about SMTC differ from the Mac side enough to shape this file.
//
// A desktop process cannot use SystemMediaTransportControls::GetForCurrentView()
// -- that needs a CoreWindow, which only UWP has. The supported route is the
// ISystemMediaTransportControlsInterop COM interface off the class's activation
// factory, whose GetForWindow() binds the controls to a top-level HWND of this
// process. That HWND is now handed in on construction. It used to be hunted for:
// this object was built while the main window still was, so no native window
// existed yet, and the file carried a deferred-acquisition-and-retry path for the
// rest of its life. Creating it after the window instead deleted about fifty
// lines and a failure mode.
//
// SMTC has no toggle button. The keyboard's play/pause key arrives as *either*
// Play or Pause, chosen by the PlaybackStatus we last reported -- so unlike
// MPRemoteCommandCenter's togglePlayPauseCommand, keeping the status accurate is
// not cosmetic, it is what makes the key do the right thing. The window already
// treats playRequested and pauseRequested idempotently, so they map directly.
//
// Events arrive on a Windows thread pool thread, not the user interface's. Every
// handler therefore goes through publishOnUiThread(), which is what the injected
// dispatcher exists for; Qt's queued connections used to do this invisibly.

// Ahead of everything else deliberately: these bring in <windows.h>, and the
// order used to matter because Qt's macros must not be in scope when the SDK is
// parsed. It no longer does, and the ordering is kept because C++/WinRT is still
// happier seeing the SDK first.
#include <windows.h>

#include <shcore.h>
#include <shlwapi.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include "xpcog/platform/MediaIntegration.hpp"

#include "WinString.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace xpcog::platform {
namespace {

using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::PlaybackPositionChangeRequestedEventArgs;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;
using winrt::Windows::Storage::Streams::IRandomAccessStream;
using winrt::Windows::Storage::Streams::RandomAccessStreamReference;

[[nodiscard]] winrt::hstring toHString(const std::string& utf8) {
    const std::wstring wide = toWide(utf8);
    return winrt::hstring{wide.c_str(), static_cast<std::uint32_t>(wide.size())};
}

/// Negatives and NaN both collapse to zero, which is how SMTC spells "unknown".
[[nodiscard]] TimeSpan toTimeSpan(double seconds) {
    if (!(seconds > 0.0)) {
        return TimeSpan::zero();
    }
    return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(seconds));
}

[[nodiscard]] double toSeconds(TimeSpan span) {
    return std::chrono::duration<double>(span).count();
}

/// The artwork as a WinRT stream, synchronously.
///
/// The obvious route -- InMemoryRandomAccessStream plus a DataWriter -- is
/// async, and the only way to finish it here would be to block the UI thread on
/// an STA, which C++/WinRT rightly objects to. Wrapping a plain COM memory
/// stream sidesteps the async API altogether: SHCreateMemStream copies the
/// bytes, so the caller's vector does not have to outlive this call, and no
/// temporary file is involved.
///
/// These are the image's own encoded bytes, straight out of the file the music
/// came in. WIC decodes JPEG and PNG alike, so the decode-to-image-and-re-encode-
/// as-PNG round trip this used to do bought nothing and has gone.
[[nodiscard]] IRandomAccessStream toStream(const std::vector<std::byte>& artwork) {
    if (artwork.empty()) {
        return nullptr;
    }

    winrt::com_ptr<IStream> memory;
    memory.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(artwork.data()),
                                    static_cast<UINT>(artwork.size())));
    if (!memory) {
        return nullptr;
    }

    IRandomAccessStream stream{nullptr};
    if (FAILED(CreateRandomAccessStreamOverStream(
            memory.get(), BSOS_DEFAULT, winrt::guid_of<IRandomAccessStream>(),
            winrt::put_abi(stream)))) {
        return nullptr;
    }
    return stream;
}

void report(const winrt::hresult_error& error) {
    // Once. A failure here is never worth a warning per transport tick.
    static bool reported = false;
    if (reported) {
        return;
    }
    reported = true;

    const winrt::hstring message = error.message();
    std::fprintf(stderr, "SMTC unavailable, continuing without it: %s (0x%08lX)\n",
                 toUtf8(std::wstring_view{message.c_str(), message.size()}).c_str(),
                 static_cast<unsigned long>(error.code()));
}

class WindowsMediaIntegration final : public MediaIntegration {
public:
    WindowsMediaIntegration(Dispatcher dispatch, HWND window)
        : MediaIntegration(std::move(dispatch)) {
        try {
            auto interop =
                winrt::get_activation_factory<SystemMediaTransportControls,
                                              ISystemMediaTransportControlsInterop>();
            SystemMediaTransportControls controls{nullptr};
            winrt::check_hresult(interop->GetForWindow(
                window, winrt::guid_of<SystemMediaTransportControls>(),
                winrt::put_abi(controls)));

            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(true);
            controls.IsStopEnabled(true);
            controls.IsNextEnabled(true);
            controls.IsPreviousEnabled(true);

            // Explicitly off, as on macOS: enabled, they show up as controls
            // that do nothing.
            controls.IsRewindEnabled(false);
            controls.IsFastForwardEnabled(false);
            controls.IsRecordEnabled(false);
            controls.IsChannelUpEnabled(false);
            controls.IsChannelDownEnabled(false);

            buttonToken_ = controls.ButtonPressed(
                [this](const SystemMediaTransportControls&,
                       const SystemMediaTransportControlsButtonPressedEventArgs& args) {
                    // Thread pool thread. Copy the button out and hand the rest
                    // to the user interface's thread.
                    deliverButton(args.Button());
                });

            positionToken_ = controls.PlaybackPositionChangeRequested(
                [this](const SystemMediaTransportControls&,
                       const PlaybackPositionChangeRequestedEventArgs& args) {
                    publishOnUiThread(seekRequested,
                                      toSeconds(args.RequestedPlaybackPosition()));
                });

            controls_ = controls;
        } catch (const winrt::hresult_error& error) {
            // Not fatal. Activation failing is a property of the process -- COM
            // not initialised, or SMTC unavailable -- and a player that refused
            // to start over a missing overlay would be a worse answer.
            report(error);
        }
    }

    ~WindowsMediaIntegration() override {
        if (!controls_) {
            return;
        }
        // Before anything else: a handler that fires during destruction would
        // capture a half-dead `this`.
        controls_.ButtonPressed(buttonToken_);
        controls_.PlaybackPositionChangeRequested(positionToken_);
        try {
            clearControls();
        } catch (const winrt::hresult_error&) {
            // Nothing useful to do while unwinding, and a throwing destructor
            // would be worse than a stale entry the shell drops on exit anyway.
        }
    }

    void setNowPlaying(const NowPlayingInfo& info) override {
        // Kept whether or not the controls exist: it is where setPlaybackState()
        // reads the duration from.
        info_ = info;
        if (!controls_) {
            return;
        }
        try {
            applyNowPlaying();
        } catch (const winrt::hresult_error& error) {
            report(error);
        }
    }

    void setPlaybackState(bool playing, bool paused, double position) override {
        if (!controls_) {
            return;
        }
        try {
            controls_.PlaybackStatus(!playing ? MediaPlaybackStatus::Stopped
                                     : paused ? MediaPlaybackStatus::Paused
                                              : MediaPlaybackStatus::Playing);
            applyTimeline(position);
        } catch (const winrt::hresult_error& error) {
            report(error);
        }
    }

    void clear() override {
        info_ = NowPlayingInfo{};
        if (!controls_) {
            return;
        }
        try {
            clearControls();
        } catch (const winrt::hresult_error& error) {
            report(error);
        }
    }

private:
    void applyNowPlaying() {
        auto updater = controls_.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);

        auto music = updater.MusicProperties();
        music.Title(toHString(info_.title));
        music.Artist(toHString(info_.artist));
        music.AlbumTitle(toHString(info_.album));

        // Assigned in both directions: left alone, the previous track's art
        // stays on screen under the new title.
        if (IRandomAccessStream stream = toStream(info_.artwork)) {
            updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(stream));
        } else {
            updater.Thumbnail(nullptr);
        }

        updater.Update();
        applyTimeline(info_.position);
    }

    void applyTimeline(double position) {
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(TimeSpan::zero());
        timeline.Position(toTimeSpan(position));
        timeline.EndTime(toTimeSpan(info_.duration));

        // Not optional, and not obviously load-bearing: without a seek range
        // SMTC never raises PlaybackPositionChangeRequested, so dragging the
        // overlay's scrubber silently does nothing. A duration of zero -- a
        // stream -- correctly yields no timeline and no seeking.
        timeline.MinSeekTime(TimeSpan::zero());
        timeline.MaxSeekTime(toTimeSpan(info_.duration));

        controls_.UpdateTimelineProperties(timeline);
    }

    void clearControls() {
        auto updater = controls_.DisplayUpdater();
        updater.ClearAll();
        updater.Update();
        // Closed rather than Stopped: Stopped keeps the entry on screen with
        // dead controls, which is worse than having none.
        controls_.PlaybackStatus(MediaPlaybackStatus::Closed);
    }

    /// Thread pool thread; every branch hands off to the interface's.
    ///
    /// Not called `dispatch`: the constructor takes a Dispatcher by that name,
    /// and inside its lambdas the parameter wins the lookup.
    void deliverButton(SystemMediaTransportControlsButton button) {
        switch (button) {
            case SystemMediaTransportControlsButton::Play:
                publishOnUiThread(playRequested);
                break;
            case SystemMediaTransportControlsButton::Pause:
                publishOnUiThread(pauseRequested);
                break;
            case SystemMediaTransportControlsButton::Stop:
                publishOnUiThread(stopRequested);
                break;
            case SystemMediaTransportControlsButton::Next:
                publishOnUiThread(nextRequested);
                break;
            case SystemMediaTransportControlsButton::Previous:
                publishOnUiThread(previousRequested);
                break;
            default:
                // Record, FastForward, Rewind, ChannelUp, ChannelDown -- all
                // disabled above, so reaching here means the shell sent
                // something unasked for.
                break;
        }
    }

    SystemMediaTransportControls controls_{nullptr};
    winrt::event_token           buttonToken_{};
    winrt::event_token           positionToken_{};
    NowPlayingInfo               info_{};
};

}  // namespace

std::unique_ptr<MediaIntegration> MediaIntegration::create(Dispatcher dispatch,
                                                           void* nativeWindow) {
    if (nativeWindow == nullptr) {
        // No window means no SMTC to bind to. The base class is the honest
        // answer rather than an implementation that can never acquire anything.
        return std::make_unique<MediaIntegration>(std::move(dispatch));
    }
    return std::make_unique<WindowsMediaIntegration>(std::move(dispatch),
                                                     static_cast<HWND>(nativeWindow));
}

}  // namespace xpcog::platform
