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
//
// The waveform is a mode of the same widget rather than a second one, for the
// same reason: it is the same gesture on the same geometry, with the track's
// shape drawn where the groove was. In that mode the bar is taller, the played
// part of the shape takes the accent colour, a thin playhead replaces the
// thumb, and buckets the analyser has not reached yet are drawn as the plain
// groove -- so the shape visibly fills in from the left the first time a track
// is heard.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/Waveform.hpp"

#include <wx/window.h>

#include <memory>
#include <string>

class wxGraphicsContext;

namespace xpcog::app {

/// The transport's clock: `m:ss`, or `h:mm:ss` once past an hour.
///
/// **Truncated, not rounded**, which is the whole reason this is a function with
/// a comment rather than three copies of a one-liner. At 0.9 s the listener is
/// still inside the first second, and a clock reading 0:01 there is half a second
/// early for the whole track -- most visibly at a track's start, where the label
/// appears at 0:01 before a second has played and then sits there until the
/// playhead catches up, which reads as the clock starting late and freezing.
/// Cog truncates in both places it formats a time: `(long)value` in
/// PositionSlider.m, `(unsigned)[object doubleValue]` in SecondsFormatter.m.
[[nodiscard]] std::string formatClock(double seconds);

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

    /// Taller, and drawing the track's shape when one has been given. Off is
    /// the plain bar, drawn exactly as it was before there was a mode. Changes
    /// the minimum size, so the owner lays out again afterwards.
    void setWaveformMode(bool on);
    [[nodiscard]] bool waveformMode() const noexcept { return waveformMode_; }

    /// How the shape is drawn. `rectified` stands the shape on the bottom edge
    /// rather than mirroring it about the centre, which is the other way a
    /// waveform overview is commonly drawn and gives each level twice the
    /// height. `logarithmic` maps levels in decibels rather than linearly, so
    /// quiet material -- classical, a spoken word -- is a shape rather than a
    /// line. Both are drawing choices only: the buckets are the same.
    struct WaveformStyle {
        bool rectified   = false;
        bool logarithmic = false;

        [[nodiscard]] friend bool operator==(const WaveformStyle&, const WaveformStyle&) = default;
    };
    void setWaveformStyle(WaveformStyle style);
    [[nodiscard]] WaveformStyle waveformStyle() const noexcept { return style_; }

    /// The shape to draw, or nothing: a stream, a track that cannot be
    /// summarised, or one not started yet, all of which draw the plain groove.
    /// A partial summary is drawn as far as it goes. Only looked at in
    /// waveform mode, but kept either way, so toggling the mode back on does
    /// not have to wait for another.
    void setWaveform(std::shared_ptr<const WaveformSummary> summary);
    [[nodiscard]] const std::shared_ptr<const WaveformSummary>& waveform() const noexcept {
        return waveform_;
    }

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

    /// The plain bar: groove, fill, thumb.
    void paintPlain(wxGraphicsContext& gc, double left, double width, double centreY);
    /// The shape, the playhead, and the plain groove past what has been analysed.
    void paintWaveform(wxGraphicsContext& gc, double left, double width, double centreY,
                       double halfHeight);

    double duration_  = 0.0;
    double position_  = 0.0;
    bool   scrubbing_ = false;

    bool                                   waveformMode_ = false;
    WaveformStyle                          style_;
    std::shared_ptr<const WaveformSummary> waveform_;
};

}  // namespace xpcog::app
