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
// process. XPCog therefore cannot create its controls in the constructor: this
// object is built while MainWindow is still being constructed, so no native
// window exists yet. Acquisition is deferred to the first call that has
// something to say, and retried until a window exists.
//
// SMTC has no toggle button. The keyboard's play/pause key arrives as *either*
// Play or Pause, chosen by the PlaybackStatus we last reported -- so unlike
// MPRemoteCommandCenter's togglePlayPauseCommand, keeping the status accurate is
// not cosmetic, it is what makes the key do the right thing. MainWindow already
// treats playRequested and pauseRequested idempotently, so they map directly.
//
// Events arrive on a Windows thread pool thread, not this object's. Every
// handler therefore hops to the object's thread before touching Qt.

// Ahead of the Qt headers deliberately: these bring in <windows.h>, and Qt's
// `signals`/`slots`/`emit` macros must not be in scope when the SDK is parsed.
// NOMINMAX and WIN32_LEAN_AND_MEAN come from XPCog::warnings.
#include <windows.h>

#include <shcore.h>
#include <shlwapi.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include "xpcog/platform/MediaIntegration.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QGuiApplication>
#include <QMetaObject>
#include <QString>
#include <QWindow>
#include <QtGlobal>

#include <chrono>

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

/// QString is already UTF-16, so this is a copy rather than a conversion.
[[nodiscard]] winrt::hstring toHString(const QString& text) {
    return winrt::hstring{reinterpret_cast<const wchar_t*>(text.utf16()),
                          static_cast<std::uint32_t>(text.size())};
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
/// bytes, so the QByteArray does not have to outlive this call, and no temporary
/// file is involved.
[[nodiscard]] IRandomAccessStream toStream(const QImage& image) {
    if (image.isNull()) {
        return nullptr;
    }

    QByteArray png;
    QBuffer    buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return nullptr;
    }

    winrt::com_ptr<IStream> memory;
    memory.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(png.constData()),
                                    static_cast<UINT>(png.size())));
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

class WindowsMediaIntegration final : public MediaIntegration {
public:
    explicit WindowsMediaIntegration(QObject* parent) : MediaIntegration(parent) {}

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
        // Kept whether or not the controls exist yet: it is both what a
        // late-arriving window needs replayed, and where setPlaybackState()
        // reads the duration from.
        info_ = info;
        if (!ensureControls()) {
            return;
        }
        try {
            applyNowPlaying();
        } catch (const winrt::hresult_error& error) {
            report(error);
        }
    }

    void setPlaybackState(bool playing, bool paused, double position) override {
        if (!ensureControls()) {
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
    /// A top-level native window of this process, or null if there is not one
    /// yet. `handle()` rather than `winId()`: the latter would *create* the
    /// native window as a side effect of asking about it.
    [[nodiscard]] static HWND topLevelWindow() {
        const QList<QWindow*> windows = QGuiApplication::topLevelWindows();
        for (QWindow* window : windows) {
            if (window != nullptr && window->handle() != nullptr) {
                return reinterpret_cast<HWND>(window->winId());
            }
        }
        return nullptr;
    }

    /// True when `controls_` is usable. Cheap on every call after the first.
    bool ensureControls() {
        if (controls_) {
            return true;
        }
        if (unavailable_) {
            return false;
        }

        // Not a failure: the window simply is not up yet. Deliberately does not
        // latch, so the next call tries again.
        HWND window = topLevelWindow();
        if (window == nullptr) {
            return false;
        }

        try {
            auto interop = winrt::get_activation_factory<SystemMediaTransportControls,
                                                         ISystemMediaTransportControlsInterop>();
            SystemMediaTransportControls controls{nullptr};
            winrt::check_hresult(
                interop->GetForWindow(window, winrt::guid_of<SystemMediaTransportControls>(),
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
                    // to the object's own thread.
                    const SystemMediaTransportControlsButton button = args.Button();
                    QMetaObject::invokeMethod(
                        this, [this, button] { dispatch(button); }, Qt::QueuedConnection);
                });

            positionToken_ = controls.PlaybackPositionChangeRequested(
                [this](const SystemMediaTransportControls&,
                       const PlaybackPositionChangeRequestedEventArgs& args) {
                    const double seconds = toSeconds(args.RequestedPlaybackPosition());
                    QMetaObject::invokeMethod(
                        this, [this, seconds] { emit seekRequested(seconds); },
                        Qt::QueuedConnection);
                });

            controls_ = controls;
        } catch (const winrt::hresult_error& error) {
            // Latched. Activation failing is a property of the process -- COM
            // not initialised, or SMTC unavailable -- not of this moment, so
            // retrying per tick would only repeat the warning forever.
            unavailable_ = true;
            report(error);
            return false;
        }

        // Whatever was already playing when the window finally appeared.
        try {
            applyNowPlaying();
        } catch (const winrt::hresult_error& error) {
            report(error);
        }
        return true;
    }

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

    /// Object's own thread.
    void dispatch(SystemMediaTransportControlsButton button) {
        switch (button) {
            case SystemMediaTransportControlsButton::Play:
                emit playRequested();
                break;
            case SystemMediaTransportControlsButton::Pause:
                emit pauseRequested();
                break;
            case SystemMediaTransportControlsButton::Stop:
                emit stopRequested();
                break;
            case SystemMediaTransportControlsButton::Next:
                emit nextRequested();
                break;
            case SystemMediaTransportControlsButton::Previous:
                emit previousRequested();
                break;
            default:
                // Record, FastForward, Rewind, ChannelUp, ChannelDown -- all
                // disabled above, so reaching here means the shell sent
                // something unasked for.
                break;
        }
    }

    static void report(const winrt::hresult_error& error) {
        // Once. A failure here is never worth a warning per transport tick.
        static bool reported = false;
        if (reported) {
            return;
        }
        reported = true;
        qWarning("SMTC unavailable, continuing without it: %s (0x%08lX)",
                 qUtf8Printable(QString::fromWCharArray(error.message().c_str())),
                 static_cast<unsigned long>(error.code()));
    }

    SystemMediaTransportControls controls_{nullptr};
    winrt::event_token          buttonToken_{};
    winrt::event_token          positionToken_{};
    NowPlayingInfo              info_{};
    bool                        unavailable_ = false;
};

}  // namespace

MediaIntegration* MediaIntegration::create(QObject* parent) {
    return new WindowsMediaIntegration(parent);
}

}  // namespace xpcog::platform
