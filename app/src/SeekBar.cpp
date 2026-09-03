#include "SeekBar.hpp"

#include "xpcog/platform/AccentColour.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

#include <algorithm>
#include <memory>

namespace xpcog::app {
namespace {

/// The groove's thickness, and the thumb's radius, in device-independent pixels.
constexpr int kGrooveHeight = 4;
constexpr int kThumbRadius  = 6;

/// Room either side so the thumb is not clipped at the ends. Everything the bar
/// draws is inset by this, and every position maps into what is left.
constexpr int kMargin = kThumbRadius + 1;

/// How hard the two outlines are drawn, out of 255. The thumb's is the one that
/// has to survive being read against the accent colour behind it as well as
/// against the window, so it is much the stronger of the two.
constexpr unsigned char kGrooveOutlineAlpha = 90;
constexpr unsigned char kThumbOutlineAlpha  = 210;

/// The window's foreground colour at a given strength.
///
/// The foreground is the one colour guaranteed to contrast with the background,
/// in both appearances and in whatever theme the user is running -- it is what
/// text is drawn in, and text has to be readable. Deriving the outlines from it
/// rather than naming a grey is what keeps them visible in dark mode, where a
/// fixed grey is either invisible or a stripe.
[[nodiscard]] wxColour outline(unsigned char alpha) {
    const wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    return wxColour(fg.Red(), fg.Green(), fg.Blue(), alpha);
}

/// The desktop's accent colour, or the toolkit's selection colour where there is
/// none to be had.
///
/// wxSYS_COLOUR_HIGHLIGHT is what this used to use outright, and on macOS that is
/// wrong in a way that is easy to miss: it is a pale derivative of the accent,
/// meant to sit *behind* text, so the bar came out a washed-out version of the
/// colour every other slider on the screen was drawn in. On Linux the two are the
/// same value, which is why the fallback is a real answer rather than a stopgap.
[[nodiscard]] wxColour accent() {
    if (const std::optional<platform::AccentRgb> rgb = platform::accentColour()) {
        return wxColour(rgb->red, rgb->green, rgb->blue);
    }
    return wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
}

}  // namespace

SeekBar::SeekBar(wxWindow* parent, wxWindowID id)
    : wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    // wx does not double-buffer on MSW, and an unbuffered custom paint flickers
    // visibly at four updates a second.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(120, (2 * kThumbRadius) + 4)));

    Bind(wxEVT_PAINT, &SeekBar::onPaint, this);
    Bind(wxEVT_LEFT_DOWN, &SeekBar::onMouseDown, this);
    Bind(wxEVT_MOTION, &SeekBar::onMouseMove, this);
    Bind(wxEVT_LEFT_UP, &SeekBar::onMouseUp, this);
    // Not optional: wx asserts if a window that captured the mouse does not
    // handle losing it, and the capture can be taken away by anything from an
    // Alt-Tab to a modal dialog opening.
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &SeekBar::onCaptureLost, this);

    // Every colour this draws is read at paint time, so a repaint is the whole
    // of what an appearance change needs -- but something has to ask for it, and
    // a bar sitting at 0:00 has no other reason to redraw. Bound here rather than
    // by the window, for the reason in the header: the mini player is a second
    // user of this widget and must not have to remember.
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
        event.Skip();
        Refresh();
    });
}

void SeekBar::setDuration(double seconds) {
    const double clamped = seconds > 0.0 ? seconds : 0.0;
    if (duration_ == clamped) {
        return;
    }
    duration_ = clamped;
    if (position_ > duration_) {
        position_ = duration_;
    }
    Refresh();
}

void SeekBar::setPosition(double seconds) {
    if (scrubbing_) {
        // The cursor owns the thumb until it is released. Without this the
        // transport's tick fights the drag and the thumb jitters between the two.
        return;
    }
    const double clamped = std::clamp(seconds, 0.0, duration_);
    if (position_ == clamped) {
        return;
    }
    position_ = clamped;
    Refresh();
}

double SeekBar::positionAt(int x) const {
    const int width = GetClientSize().GetWidth() - (2 * FromDIP(kMargin));
    if (width <= 0 || duration_ <= 0.0) {
        return 0.0;
    }
    const double fraction =
        std::clamp(static_cast<double>(x - FromDIP(kMargin)) / width, 0.0, 1.0);
    return fraction * duration_;
}

int SeekBar::thumbCentre() const {
    const int margin = FromDIP(kMargin);
    const int width  = GetClientSize().GetWidth() - (2 * margin);
    if (width <= 0) {
        return margin;
    }
    const double fraction = duration_ > 0.0 ? position_ / duration_ : 0.0;
    return margin + static_cast<int>(fraction * width);
}

void SeekBar::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        return;
    }
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize size    = GetClientSize();
    const int    margin  = FromDIP(kMargin);
    const int    groove  = FromDIP(kGrooveHeight);
    const int    radius  = FromDIP(kThumbRadius);
    const double centreY = size.GetHeight() / 2.0;
    const double left    = margin;
    const double width   = std::max(0, size.GetWidth() - (2 * margin));

    const wxColour trackColour = wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
    const wxColour fillColour  = accent();
    const wxColour thumbColour = duration_ > 0.0 ? fillColour : trackColour;

    // The groove, full width, rounded so the ends do not read as cut off.
    //
    // Outlined now rather than drawn as a bare fill. 3DSHADOW against a window
    // background is a few points of luminance on both platforms and in both
    // appearances, so the unfilled remainder of the bar was very nearly invisible
    // -- which made the bar look like it ended at the thumb.
    //
    // Inset by half the pen width, because wxGraphicsContext strokes centred on
    // the path: without it half of the line is drawn outside the rectangle and
    // the groove comes out a pixel taller than kGrooveHeight at each end.
    const double hairline = FromDIP(1);
    const double top      = centreY - (groove / 2.0);
    gc->SetBrush(wxBrush(trackColour));
    gc->SetPen(wxPen(outline(kGrooveOutlineAlpha), hairline));
    gc->DrawRoundedRectangle(left + (hairline / 2.0), top + (hairline / 2.0),
                             std::max(0.0, width - hairline),
                             std::max(0.0, groove - hairline), groove / 2.0);

    if (duration_ <= 0.0) {
        // A stream: no length, so nothing to fill and no thumb to aim with.
        // Drawing an empty groove says that more honestly than a thumb parked at
        // zero, which reads as a track that will not start.
        return;
    }

    // The elapsed portion, over the groove and inside its outline, so the two
    // meet without a seam and the outline stays one unbroken shape around the
    // whole bar rather than stopping where the fill starts.
    const double filled = static_cast<double>(thumbCentre()) - left;
    gc->SetBrush(wxBrush(fillColour));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRoundedRectangle(left + (hairline / 2.0), top + (hairline / 2.0),
                             std::max(0.0, filled - hairline),
                             std::max(0.0, groove - hairline), groove / 2.0);

    // The thumb last, so it sits over both. Its outline is the strong one: it has
    // to read against the accent colour under it as well as against the window
    // behind it, and it is the part the eye is aiming at during a drag.
    // FromDIP takes an int, so the half-pixel comes from scaling its result:
    // 1.5 at 1x and 3 at 2x, which wxGraphicsContext strokes happily because its
    // pen widths are doubles. FromDIP(1.5) would silently truncate to 1.
    const double thumbPen = FromDIP(1) * 1.5;
    gc->SetBrush(wxBrush(thumbColour));
    gc->SetPen(wxPen(outline(kThumbOutlineAlpha), thumbPen));
    const double thumbSize = (2.0 * radius) - thumbPen;
    gc->DrawEllipse(thumbCentre() - radius + (thumbPen / 2.0),
                    centreY - radius + (thumbPen / 2.0), thumbSize, thumbSize);
}

void SeekBar::onMouseDown(wxMouseEvent& event) {
    if (duration_ <= 0.0) {
        event.Skip();
        return;
    }
    scrubbing_ = true;
    CaptureMouse();
    // Straight to where the click landed, rather than paging towards it. This is
    // the behaviour the whole widget exists for.
    position_ = positionAt(event.GetX());
    scrubbed.publish(position_);
    Refresh();
}

void SeekBar::onMouseMove(wxMouseEvent& event) {
    if (!scrubbing_) {
        event.Skip();
        return;
    }
    position_ = positionAt(event.GetX());
    scrubbed.publish(position_);
    Refresh();
}

void SeekBar::onMouseUp(wxMouseEvent& event) {
    if (!scrubbing_) {
        event.Skip();
        return;
    }
    position_ = positionAt(event.GetX());
    stopScrubbing();
    // On release, not on every motion: seeking is expensive and a drag across a
    // long track would ask the engine for a hundred of them.
    seekRequested.publish(position_);
    Refresh();
}

void SeekBar::onCaptureLost(wxMouseCaptureLostEvent&) {
    // No seek published. The gesture was interrupted rather than completed, and
    // jumping the track because a dialog stole focus is not what was asked for.
    scrubbing_ = false;
    Refresh();
}

void SeekBar::stopScrubbing() {
    scrubbing_ = false;
    if (HasCapture()) {
        ReleaseMouse();
    }
}

}  // namespace xpcog::app
