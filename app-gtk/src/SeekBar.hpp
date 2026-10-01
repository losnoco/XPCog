// The transport's position bar. The counterpart of app-winui/src/SeekBar.hpp, with
// the mini player as its second user, so both behaviours live in the widget
// rather than in a window.
//
// **Plain, it is GTK's own slider.** A GtkScale with the primary button
// warping to the click -- GTK 4's default, `gtk-primary-button-warps-slider`
// -- is what the wx bar is drawn by hand to be: a click jumps rather than
// pages. It also brings what a painted bar has to fake: the theme's look and
// accent, keyboard stepping and an accessible value. What it lacks is a
// "let go": its drags report every step through `change-value`. A legacy
// controller in the capture phase sees the press and release the scale's own
// gestures consume, so a drag is a scrub, the release is the one seek, and a
// keyboard step -- no press behind it -- seeks at once.
//
// **The waveform is painted**, because no stock slider draws a track's shape
// where the groove is: a GtkDrawingArea with one drag gesture, begin being
// the click, updates the scrub and end the seek, and a cancelled gesture
// seeking nothing. Until a shape is known -- a stream, a track the analyser
// has not reached -- waveform mode shows the plain slider, centred in the
// taller strip, so the bar is never blank.

#pragma once

#include "Glib.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/Waveform.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpcog::gtk {

class SeekBar {
public:
    SeekBar();
    ~SeekBar();

    SeekBar(const SeekBar&)            = delete;
    SeekBar& operator=(const SeekBar&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// The track's length. Zero disables the bar, which is what a stream
    /// with no known duration wants: nothing to seek within, so nothing to drag.
    void setDuration(double seconds);
    [[nodiscard]] double duration() const noexcept { return duration_; }

    /// Where playback is. Ignored while the user is dragging, so an update
    /// arriving mid-scrub cannot yank the thumb out from under the cursor.
    void setPosition(double seconds);

    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

    /// Taller, and drawing the track's shape when one has been given. Off is
    /// the plain bar. Changes the size request, so the row lays out again.
    void setWaveformMode(bool on);
    [[nodiscard]] bool waveformMode() const noexcept { return waveformMode_; }

    /// How the shape is drawn; the wx SeekBar::WaveformStyle, with GdkRGBA
    /// for the two colours the listener may choose.
    struct WaveformStyle {
        bool                   rectified   = false;
        bool                   logarithmic = false;
        int                    height      = 28;
        std::optional<GdkRGBA> played;
        std::optional<GdkRGBA> unplayed;
    };
    void setWaveformStyle(const WaveformStyle& style);

    /// The style the settings describe.
    [[nodiscard]] static WaveformStyle styleFrom(const Settings& settings);

    /// The shape to draw, or nothing. Kept whether or not the mode is on, so
    /// toggling the mode back on does not have to wait for another.
    void setWaveform(std::shared_ptr<const WaveformSummary> summary);
    [[nodiscard]] const std::shared_ptr<const WaveformSummary>& waveform() const noexcept {
        return waveform_;
    }

    /// The user let go. Carries seconds.
    Signal<double> seekRequested;

    /// The thumb moved while held, for the clock to follow.
    Signal<double> scrubbed;

private:
    void draw(cairo_t* cr, int width, int height);
    /// Shows the slider or the painting, whichever the mode and the shape say.
    void choose();
    /// The slider's range and value, from duration_ and position_.
    void syncScale();
    void paintWaveform(cairo_t* cr, double left, double width, double centreY, double halfHeight);

    [[nodiscard]] double positionAt(double x) const;
    [[nodiscard]] double thumbCentre() const;

    /// Owned as well as borrowed: the draw function holds `this`, and
    /// detaching it in the destructor needs the widget still there, whichever
    /// of the two the window tears down first.
    GObjectPtr<GtkWidget>   owned_;
    GtkWidget*              root_  = nullptr;  ///< A GtkStack of the two.
    GtkWidget*              scale_ = nullptr;
    GtkWidget*              area_  = nullptr;
    std::vector<Connection> connections_;

    double duration_  = 0.0;
    double position_  = 0.0;
    bool   scrubbing_ = false;
    /// A button or touch is down on the slider.
    bool pressed_ = false;

    bool                                   waveformMode_ = false;
    WaveformStyle                          style_;
    std::shared_ptr<const WaveformSummary> waveform_;
};

}  // namespace xpcog::gtk
