// The mini player. Port of Cog's `miniWindow` (Base.lproj/MainMenu.xib, driven by
// AppController's -setMiniMode:).
//
// It is a *mode*, not a second window: Cog closes the main window and opens this
// one, and `toggleMiniMode:` swaps them back. That is worth preserving, because a
// mini player shown *beside* the full window is a different thing -- the point is to
// get the playlist off the screen.
//
// What it is not is a straight visual copy, and the reason is structural. Cog's
// mini window has **no content view at all**: it is an NSWindow whose entire body is
// a unified toolbar in the title bar, which is why AppController keeps setting its
// content size to a height of zero. Qt has no equivalent -- a title bar is the
// window manager's on every platform this targets -- so the same controls live in a
// single compact row below a normal title bar. The intent survives; the mechanism
// cannot.
//
// Cog's `miniPlusWindow` -- the larger variant with artwork -- is deliberately not
// ported.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <QWidget>

class QLabel;
class QSlider;

namespace xpcog::app {

class ActionRegistry;
class PlaybackController;
class SeekSlider;

class MiniWindow : public QWidget {
    Q_OBJECT

public:
    /// `actions` supplies the transport commands, so the buttons here are the same
    /// QActions the menu bar and the tray use -- one enabled state, one shortcut,
    /// one place to change what Play does.
    MiniWindow(const ActionRegistry& actions, PlaybackController& playback,
               Settings& settings, QWidget* parent = nullptr);

    /// The track, shown in the window title as Cog does (title plus subtitle).
    void setNowPlaying(const QString& title, const QString& artist);

    void setPosition(double seconds, double duration);
    void setPlaybackState(bool playing, bool paused);

    /// Reads the volume back from the controller. Called when the window appears,
    /// because the main window's slider may have moved while this was hidden.
    void refreshVolume();

    /// Whether the window floats above everything else. Cog's
    /// `floatingMiniWindow`, and the reason a mini player is worth having at all
    /// for anyone who wants it visible over something else.
    void setFloating(bool floating);

signals:
    /// The user closed the window, which in a mode-based design means "go back to
    /// the full window" rather than "quit".
    void dismissed();

    /// The volume slider moved, so whatever else shows a volume can follow.
    void volumeChanged(double linear);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    PlaybackController& playback_;
    Settings&           settings_;

    SeekSlider* seekBar_ = nullptr;
    QSlider*    volume_  = nullptr;
    QLabel*     clock_   = nullptr;

    /// The audible track's length, kept so the clock can show a scrub position
    /// against it without asking the controller mid-drag.
    double duration_ = 0.0;
};

}  // namespace xpcog::app
