// The mini player. Port of Cog's `miniWindow` (Base.lproj/MainMenu.xib, driven by
// AppController's -setMiniMode:).
//
// It is a *mode*, not a second window: Cog closes the main window and opens this
// one, and `toggleMiniMode:` swaps them back. That is worth preserving, because a
// mini player shown *beside* the full window is a different thing -- the point is
// to get the playlist off the screen.
//
// What it is not is a straight visual copy, and the reason is structural. Cog's
// mini window has **no content view at all**: it is an NSWindow whose entire body
// is a unified toolbar in the title bar, which is why AppController keeps setting
// its content size to a height of zero. No cross-platform toolkit has an
// equivalent -- a title bar belongs to the window manager on every platform this
// targets -- so the same controls live in a single compact row below a normal
// title bar. The intent survives; the mechanism cannot.
//
// Cog's `miniPlusWindow` -- the larger variant with artwork -- is deliberately not
// ported.
//
// One thing wx does better than Qt here: always-on-top is a style flag that can
// be changed in place. Qt required destroying and recreating the window to add
// Qt::WindowStaysOnTopHint, which lost its position every time the setting was
// toggled.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/frame.h>

#include <string>
#include <vector>

class wxBitmapButton;
class wxSlider;
class wxStaticText;

namespace xpcog::app {

class PlaybackController;
class SeekBar;

class MiniFrame : public wxFrame {
public:
    MiniFrame(wxWindow* parent, PlaybackController& playback, Settings& settings);

    /// The track, shown in the window title as Cog does.
    void setNowPlaying(const std::string& title, const std::string& artist);

    void setPosition(double seconds, double duration);
    void setPlaybackState(bool playing, bool paused);

    /// Reads the volume back from the controller. Called when the window appears,
    /// because the main window's slider may have moved while this was hidden.
    void refreshVolume();

    /// Whether the window floats above everything else. Cog's
    /// `floatingMiniWindow`, and the reason a mini player is worth having at all
    /// for anyone who wants it visible over something else.
    void setFloating(bool floating);

    /// Re-strokes the transport glyphs after a system appearance change.
    void refreshIcons();

    /// The user closed the window, which in a mode-based design means "go back to
    /// the full window" rather than "quit".
    Signal<> dismissed;

    /// The volume slider moved, so whatever else shows a volume can follow.
    Signal<double> volumeChanged;

private:
    PlaybackController& playback_;
    Settings&           settings_;

    SeekBar*      seekBar_ = nullptr;
    wxSlider*     volume_  = nullptr;
    wxStaticText* clock_   = nullptr;

    std::vector<wxBitmapButton*> buttons_;
    /// The one whose glyph follows the transport rather than the palette.
    wxBitmapButton* playPauseButton_ = nullptr;

    /// What refreshIcons() should draw on it. Kept because the palette can
    /// change while paused, and re-stroking must not also reset the glyph.
    bool showingPause_ = false;

    /// The audible track's length, kept so the clock can show a scrub position
    /// against it without asking the controller mid-drag.
    double duration_ = 0.0;

    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::app
