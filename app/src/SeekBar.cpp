#include "SeekBar.hpp"

#include "xpcog/platform/AccentColour.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::app {
namespace {

/// The groove's thickness, and the thumb's radius, in device-independent pixels.
constexpr int kGrooveHeight = 4;
constexpr int kThumbRadius  = 6;

/// The bar's height in waveform mode, and the room left above and below the
/// shape so a full-scale bucket does not touch the edge. 28 is what fits the
/// transport row without moving the clock: tall enough for the shape to read,
/// not so tall that the row stops looking like a transport.
constexpr int kWaveformHeight  = 28;
constexpr int kWaveformPadding = 2;
/// The playhead's width in waveform mode, where it stands in for the thumb.
constexpr int kPlayheadWidth = 2;

/// The bottom of the logarithmic scale. One step of the byte a bucket is
/// stored in is 20*log10(1/255) = -48.1 dB, so a floor there is where the
/// stored resolution runs out: the quietest non-zero bucket is drawn at the
/// bottom of the scale rather than a fifth of the way up it, which a deeper
/// floor would do, and silence and near-silence still tell apart.
constexpr double kLogFloorDb = -48.0;

/// How hard the shape is drawn, out of 255. The RMS body is the darker of the
/// two so it reads as the loudness and the peak as its envelope; the played
/// pair is the accent colour at the same two strengths.
constexpr unsigned char kPeakAlpha = 55;
constexpr unsigned char kRmsAlpha  = 120;

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

std::string formatClock(double seconds) {
    // Not std::lround, and not a cast of `seconds + 0.5`: see the header. A
    // negative reading is a clock that has not started, not a time before the
    // track.
    if (!(seconds > 0.0)) {  // also catches NaN, which a bad duration can be
        return "0:00";
    }

    const auto total   = static_cast<long long>(seconds);
    const long long minutes = total / 60;
    const long long rest    = total % 60;

    const auto pad = [](long long value) {
        const std::string text = std::to_string(value);
        return text.size() < 2 ? "0" + text : text;
    };

    if (minutes >= 60) {
        return std::to_string(minutes / 60) + ":" + pad(minutes % 60) + ":" + pad(rest);
    }
    return std::to_string(minutes) + ":" + pad(rest);
}

SeekBar::SeekBar(wxWindow* parent, wxWindowID id)
    : wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    // wx does not double-buffer on MSW, and an unbuffered custom paint flickers
    // visibly at four updates a second.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    setWaveformMode(false);

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

void SeekBar::setWaveformMode(bool on) {
    waveformMode_ = on;
    SetMinSize(FromDIP(wxSize(120, on ? kWaveformHeight : (2 * kThumbRadius) + 4)));
    InvalidateBestSize();
    Refresh();
}

void SeekBar::setWaveformStyle(WaveformStyle style) {
    if (style_ == style) {
        return;
    }
    style_ = style;
    if (waveformMode_) {
        Refresh();
    }
}

void SeekBar::setWaveform(std::shared_ptr<const WaveformSummary> summary) {
    waveform_ = std::move(summary);
    if (waveformMode_) {
        Refresh();
    }
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
    const double centreY = size.GetHeight() / 2.0;
    const double left    = margin;
    const double width   = std::max(0, size.GetWidth() - (2 * margin));

    // The shape only when there is one to draw. A stream, a DSD file, or a
    // track the analyser has not reached yet gets the plain bar, centred in the
    // taller control, rather than an empty strip -- the bar is never blank.
    if (waveformMode_ && waveform_ && duration_ > 0.0 && waveform_->bucketCount > 0) {
        const double halfHeight = std::max(1.0, centreY - FromDIP(kWaveformPadding));
        paintWaveform(*gc, left, width, centreY, halfHeight);
        return;
    }
    paintPlain(*gc, left, width, centreY);
}

void SeekBar::paintPlain(wxGraphicsContext& gcRef, double left, double width, double centreY) {
    wxGraphicsContext* gc     = &gcRef;
    const int          groove = FromDIP(kGrooveHeight);
    const int          radius = FromDIP(kThumbRadius);

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

void SeekBar::paintWaveform(wxGraphicsContext& gc, double left, double width, double centreY,
                            double halfHeight) {
    const WaveformSummary& shape   = *waveform_;
    const auto             columns = static_cast<int>(width);
    if (columns <= 0) {
        return;
    }

    const wxColour trackColour = wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
    const wxColour accentColour = accent();
    const wxColour playedPeak(accentColour.Red(), accentColour.Green(), accentColour.Blue(),
                              kPeakAlpha + 60);
    const wxColour playedRms = accentColour;
    const double   hairline  = FromDIP(1);

    // Where the shape stops and the plain groove begins, in columns. A bucket
    // is drawn once every column it covers has been analysed, so the edge of
    // the shape never shows a half-filled bucket as a dip.
    const double bucketsPerColumn = static_cast<double>(shape.bucketCount) / columns;
    const int    analysedColumns  = std::clamp(
        static_cast<int>(std::floor(shape.analysed / bucketsPerColumn)), 0, columns);
    const int playedColumns = std::clamp(thumbCentre() - static_cast<int>(left), 0, columns);

    // Mirrored, the shape hangs off the centre line by up to halfHeight each
    // way. Rectified, it stands on the bottom edge and has the whole height to
    // itself -- the same bucket is drawn twice as tall, which is the point of
    // that style. `baseline` is the line the shape grows from and `amplitude`
    // how far a full-scale bucket reaches.
    const bool   rectified = style_.rectified;
    const double baseline  = rectified ? centreY + halfHeight : centreY;
    const double amplitude = rectified ? 2.0 * halfHeight : halfHeight;

    // The baseline first, under everything, so a silent stretch still reads
    // as part of the bar rather than a gap in it.
    gc.SetPen(wxPen(trackColour, hairline));
    gc.StrokeLine(left, baseline, left + width, baseline);

    // A bucket's byte as a fraction of the amplitude. Linear is the byte over
    // 255. Logarithmic is its level in decibels laid over kLogFloorDb..0, so a
    // -24 dB passage stands half way up instead of a sixteenth.
    const auto scale = [&](std::uint8_t value) {
        if (value == 0) {
            return 0.0;
        }
        const double linear = value / 255.0;
        if (!style_.logarithmic) {
            return linear;
        }
        const double db = 20.0 * std::log10(linear);
        return std::clamp(1.0 - (db / kLogFloorDb), 0.0, 1.0);
    };

    // One polygon per (level, played) pair: across the top edge of every
    // column, then back along the bottom. Filled in one go, so there are no
    // seams between columns and the antialiasing lands only on the outline.
    const auto level = [&](const std::vector<std::uint8_t>& values, int x) {
        const int first = static_cast<int>(x * bucketsPerColumn);
        const int last  = std::max(first, static_cast<int>((x + 1) * bucketsPerColumn) - 1);
        std::uint8_t peak = 0;
        for (int i = first; i <= last && i < static_cast<int>(values.size()); ++i) {
            peak = std::max(peak, values[i]);
        }
        return scale(peak) * amplitude;
    };

    const auto fill = [&](const std::vector<std::uint8_t>& values, int from, int to,
                          const wxColour& colour) {
        if (to <= from) {
            return;
        }
        wxGraphicsPath path = gc.CreatePath();
        path.MoveToPoint(left + from, baseline);
        for (int x = from; x < to; ++x) {
            const double h = level(values, x);
            path.AddLineToPoint(left + x, baseline - h);
            path.AddLineToPoint(left + x + 1, baseline - h);
        }
        path.AddLineToPoint(left + to, baseline);
        if (!rectified) {
            for (int x = to - 1; x >= from; --x) {
                const double h = level(values, x);
                path.AddLineToPoint(left + x + 1, baseline + h);
                path.AddLineToPoint(left + x, baseline + h);
            }
        }
        path.CloseSubpath();
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(colour));
        gc.FillPath(path);
    };

    const int split = std::min(playedColumns, analysedColumns);
    fill(shape.peak, 0, split, playedPeak);
    fill(shape.peak, split, analysedColumns, outline(kPeakAlpha));
    fill(shape.rms, 0, split, playedRms);
    fill(shape.rms, split, analysedColumns, outline(kRmsAlpha));

    // Past the analysis: the plain groove, with the played part filled if the
    // playhead has got there first, which it can after a seek into a long track
    // still being read.
    if (analysedColumns < columns) {
        const int    groove = FromDIP(kGrooveHeight);
        // Centred on the baseline when the shape is mirrored; standing on it
        // when the shape does, so the two meet where the analysis stopped.
        const double top    = rectified ? baseline - groove : centreY - (groove / 2.0);
        const double x      = left + analysedColumns;
        const double rest   = width - analysedColumns;
        gc.SetBrush(wxBrush(trackColour));
        gc.SetPen(wxPen(outline(kGrooveOutlineAlpha), hairline));
        gc.DrawRoundedRectangle(x + (hairline / 2.0), top + (hairline / 2.0),
                                std::max(0.0, rest - hairline),
                                std::max(0.0, groove - hairline), groove / 2.0);
        if (playedColumns > analysedColumns) {
            gc.SetBrush(wxBrush(accentColour));
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.DrawRoundedRectangle(x + (hairline / 2.0), top + (hairline / 2.0),
                                    std::max(0.0, playedColumns - analysedColumns - hairline),
                                    std::max(0.0, groove - hairline), groove / 2.0);
        }
    }

    // The playhead, full height, in the foreground colour: it has to read over
    // the accent on its left and the grey on its right, and over nothing at
    // all in a silent stretch.
    const double playhead = FromDIP(kPlayheadWidth);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(outline(kThumbOutlineAlpha)));
    gc.DrawRectangle(thumbCentre() - (playhead / 2.0), centreY - halfHeight, playhead,
                     2.0 * halfHeight);
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
