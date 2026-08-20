#include "SeekBar.hpp"

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
    const wxColour fillColour  = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    const wxColour thumbColour = duration_ > 0.0
                                     ? fillColour
                                     : wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);

    // The groove, full width, rounded so the ends do not read as cut off.
    gc->SetBrush(wxBrush(trackColour));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawRoundedRectangle(left, centreY - (groove / 2.0), width, groove,
                             groove / 2.0);

    if (duration_ <= 0.0) {
        // A stream: no length, so nothing to fill and no thumb to aim with.
        // Drawing an empty groove says that more honestly than a thumb parked at
        // zero, which reads as a track that will not start.
        return;
    }

    const double filled = static_cast<double>(thumbCentre()) - left;
    gc->SetBrush(wxBrush(fillColour));
    gc->DrawRoundedRectangle(left, centreY - (groove / 2.0), std::max(0.0, filled),
                            groove, groove / 2.0);

    gc->SetBrush(wxBrush(thumbColour));
    gc->SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW), FromDIP(1)));
    gc->DrawEllipse(thumbCentre() - radius, centreY - radius, 2.0 * radius,
                    2.0 * radius);
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
