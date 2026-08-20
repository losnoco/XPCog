// The transport's position bar.
//
// Owner-drawn, and there is no choice about it: seeking needs to know where the
// groove is and where the thumb is, and wx exposes neither. wxRendererNative
// draws headers, checkboxes, push buttons, combo boxes, collapse buttons and
// gauges -- no slider parts -- and wxSlider::GetThumbLength() is Windows-only.
// The Qt version asked QStyle for the sub-control rectangles; there is nothing to
// ask here.
//
// That turns out to be a gain rather than a cost, because a plain slider is wrong
// for seeking in two ways that both read as the control being broken, and both
// were being corrected by hand anyway:
//
//  * Clicking the groove pages by a step instead of jumping to where you clicked.
//    On a seek bar, clicking two thirds along means "go two thirds in". Drawing
//    the control ourselves makes that the only behaviour there is, rather than
//    one overridden on top of another.
//  * The time readout does not follow the thumb during a drag, so there is
//    nothing to aim with until after you let go.
//
// Both belong to the widget rather than to the window, so a second user of it
// cannot half-wire them. The mini player is that second user.

#pragma once

#include "xpcog/core/Signal.hpp"

#include <wx/window.h>

namespace xpcog::app {

class SeekBar : public wxWindow {
public:
    SeekBar(wxWindow* parent, wxWindowID id);

    /// The track's length. Zero disables the bar, which is what a stream with no
    /// known duration wants: nothing to seek within, so nothing to drag.
    void setDuration(double seconds);
    [[nodiscard]] double duration() const noexcept { return duration_; }

    /// Where playback is. Ignored while the user is dragging, so an update
    /// arriving mid-scrub cannot yank the thumb out from under the cursor.
    void setPosition(double seconds);

    /// True while the user holds the thumb. The window reads this to stop
    /// position updates fighting the cursor.
    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

    /// The user let go. Carries seconds, so nothing else has to know how the bar
    /// is scaled.
    Signal<double> seekRequested;

    /// The thumb moved while held, for the clock to follow.
    Signal<double> scrubbed;

private:
    void onPaint(wxPaintEvent& event);
    void onMouseDown(wxMouseEvent& event);
    void onMouseMove(wxMouseEvent& event);
    void onMouseUp(wxMouseEvent& event);
    void onCaptureLost(wxMouseCaptureLostEvent& event);

    /// Seconds at pixel `x`, clamped to the track.
    [[nodiscard]] double positionAt(int x) const;

    /// Where the thumb's centre sits for the current position.
    [[nodiscard]] int thumbCentre() const;

    void stopScrubbing();

    double duration_  = 0.0;
    double position_  = 0.0;
    bool   scrubbing_ = false;
};

}  // namespace xpcog::app
