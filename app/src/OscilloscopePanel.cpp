#include "OscilloscopePanel.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/Oscilloscope.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/translation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>

namespace xpcog::app {
namespace {

/// Room above and below a full-scale trace, so a clipped run sits inside the
/// band rather than on its edge.
constexpr int kPadding = 2;

/// Frames kept clear behind the paced reader's runway when the fetch is
/// clamped to the tap. Generous rather than exact, because a fetch the writer
/// laps mid-copy is a frame dropped, and the tap is big.
constexpr std::size_t kTapMargin = 1024;

/// The fill's strength, out of 255, and the centre line's.
constexpr unsigned char kFillAlpha   = 90;
constexpr unsigned char kCentreAlpha = 70;

/// Qt's QColor::darker(), as SpectrumPanel spells it; one function, so a copy
/// rather than a header. The overlaid mode's second trace is drawn in it.
[[nodiscard]] wxColour shade(const wxColour& colour, int factor) {
    const auto scale = [factor](unsigned char channel) {
        const int scaled = (static_cast<int>(channel) * 100) / factor;
        return static_cast<unsigned char>(std::clamp(scaled, 0, 255));
    };
    return {scale(colour.Red()), scale(colour.Green()), scale(colour.Blue())};
}

[[nodiscard]] wxColour withAlpha(const wxColour& colour, unsigned char alpha) {
    return {colour.Red(), colour.Green(), colour.Blue(), alpha};
}

struct ChannelsName {
    OscilloscopePanel::Channels channels;
    const char*                 key;
};

constexpr std::array<ChannelsName, 5> kChannelsNames = {{
    {OscilloscopePanel::Channels::Mono, "mono"},
    {OscilloscopePanel::Channels::Left, "left"},
    {OscilloscopePanel::Channels::Right, "right"},
    {OscilloscopePanel::Channels::Stacked, "stacked"},
    {OscilloscopePanel::Channels::Overlaid, "overlaid"},
}};

}  // namespace

const char* OscilloscopePanel::channelsKey(Channels channels) noexcept {
    for (const ChannelsName& name : kChannelsNames) {
        if (name.channels == channels) {
            return name.key;
        }
    }
    return "mono";
}

OscilloscopePanel::Channels OscilloscopePanel::channelsFromKey(std::string_view key) noexcept {
    for (const ChannelsName& name : kChannelsNames) {
        if (key == name.key) {
            return name.channels;
        }
    }
    return Channels::Mono;
}

OscilloscopePanel::OscilloscopePanel(wxWindow* parent, AudioTap& tap, Settings& settings)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      tap_(tap),
      settings_(settings),
      timer_(this) {
    // Not optional; see SpectrumPanel. Without it the trace flickers.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(120, 48)));
    SetSize(FromDIP(wxSize(400, 120)));

    Bind(wxEVT_PAINT, &OscilloscopePanel::onPaint, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    Bind(wxEVT_CONTEXT_MENU, &OscilloscopePanel::onContextMenu, this);
    // No wxEVT_SHOW handler, for the reason SpectrumPanel's constructor gives:
    // it fires on a window being torn down, and every path that shows or
    // hides this pane already calls setActive().

    applySettings(settings_);
}

OscilloscopePanel::~OscilloscopePanel() { timer_.Stop(); }

void OscilloscopePanel::setSampleRate(double rate) {
    sampleRate_ = rate > 0.0 ? rate : 0.0;
    cursor_.setSampleRate(sampleRate_);
}

void OscilloscopePanel::applySettings(const Settings& settings) {
    // Invalid colour text keeps what was there, as the spectrum does: the
    // fallback would be black, and a black trace on a black background looks
    // like the panel has stopped.
    if (wxColour colour; colour.Set(toWx(settings.ScopeColor()))) {
        colour_ = colour;
    }
    if (wxColour background; background.Set(toWx(settings.ScopeBackgroundColor()))) {
        background_ = background;
    }
    strokeWidth_ = std::clamp(settings.ScopeStrokeWidth(), 0.5, 6.0);
    gain_        = std::clamp(settings.ScopeGain(), 0.25, 8.0);
    windowMs_    = std::clamp(settings.ScopeWindowMs(), 5, 100);
    fill_        = settings.ScopeFill();
    trigger_     = settings.ScopeTrigger();
    logScale_    = settings.ScopeLogScale();
    channels_    = channelsFromKey(settings.ScopeChannels());

    const int frameRate = std::clamp(settings.ScopeFrameRate(), 15, 120);
    if (frameRate != frameRate_) {
        frameRate_ = frameRate;
        if (timer_.IsRunning()) {
            timer_.Start(std::max(1, 1000 / frameRate_));
        }
    }
    Refresh();
}

std::size_t OscilloscopePanel::windowFrames() const noexcept {
    if (sampleRate_ <= 0.0) {
        return 0;
    }
    return static_cast<std::size_t>(std::lround(sampleRate_ * windowMs_ / 1000.0));
}

std::size_t OscilloscopePanel::fetchFrames() const noexcept {
    // Two windows: the trigger searches the older one for where the newer
    // should start. Bounded by what the tap holds behind a paced reader, which
    // sits a chunk behind the head, with a margin so the writer cannot lap
    // the fetch while it is copied.
    const std::size_t granularity = tap_.writeGranularity();
    const std::size_t room =
        tap_.capacity() > granularity + kTapMargin ? tap_.capacity() - granularity - kTapMargin
                                                   : tap_.capacity() / 2;
    return std::min(2 * windowFrames(), room);
}

void OscilloscopePanel::setActive(bool active) {
    playing_ = active;
    if (!active) {
        haveFrame_ = false;
        Refresh();
    }
    // Visible *and* playing, or the clock stops -- a hidden pane must not be
    // reading the tap a hundred times a second for nobody.
    if (active && IsShownOnScreen()) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start(std::max(1, 1000 / frameRate_));
    } else {
        timer_.Stop();
    }
}

void OscilloscopePanel::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    const std::size_t fetch = fetchFrames();
    if (fetch < 2) {
        return;
    }
    mono_.resize(fetch);

    // Nothing has played yet, or the writer lapped the window: the last frame
    // stays up, as the spectrum's does, rather than a flicker to silence.
    if (!cursor_.read(tap_, elapsed, mono_.data(), mono_.size(), TapLane::Mono)) {
        return;
    }

    const bool wantsSides = channels_ != Channels::Mono;
    if (wantsSides) {
        left_.resize(fetch);
        right_.resize(fetch);
        // The same window, other lanes. A failure here is the writer lapping
        // between the reads, which one frame later will not happen again.
        if (!cursor_.readAgain(tap_, left_.data(), left_.size(), TapLane::Left) ||
            !cursor_.readAgain(tap_, right_.data(), right_.size(), TapLane::Right)) {
            return;
        }
    }

    // The window is the newer half; the trigger may pull its start back into
    // the older half. Run on the mix whichever channels are shown, so the two
    // sides of a stereo frame show the same instant.
    const std::size_t window = std::min(windowFrames(), fetch / 2);
    traceFrames_             = window;
    traceStart_ = trigger_ ? triggerOffset(std::span<const float>{mono_}, window) : fetch - window;
    haveFrame_  = window > 0;
    Refresh(false);
}

void OscilloscopePanel::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(background_));
    dc.Clear();

    const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        return;
    }
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const wxSize size   = GetClientSize();
    const int    width  = size.GetWidth();
    const int    height = size.GetHeight();
    if (width <= 0 || height <= 0) {
        return;
    }

    // Nothing playing, or nothing read yet: the centre line alone, so the pane
    // reads as a scope with no signal rather than an empty box.
    const float* samples = haveFrame_ ? mono_.data() + traceStart_ : nullptr;
    const std::size_t count = haveFrame_ ? traceFrames_ : 0;

    switch (channels_) {
        case Channels::Mono:
            paintTrace(*gc, samples, count, 0, height, colour_);
            break;
        case Channels::Left:
            paintTrace(*gc, haveFrame_ ? left_.data() + traceStart_ : nullptr, count, 0,
                       height, colour_);
            break;
        case Channels::Right:
            paintTrace(*gc, haveFrame_ ? right_.data() + traceStart_ : nullptr, count, 0,
                       height, colour_);
            break;
        case Channels::Stacked: {
            const int half = height / 2;
            paintTrace(*gc, haveFrame_ ? left_.data() + traceStart_ : nullptr, count, 0,
                       half, colour_);
            paintTrace(*gc, haveFrame_ ? right_.data() + traceStart_ : nullptr, count,
                       half, height - half, colour_);
            break;
        }
        case Channels::Overlaid:
            // Right first and darker, so the left -- the one most material
            // leads with -- is drawn over it in the true colour.
            paintTrace(*gc, haveFrame_ ? right_.data() + traceStart_ : nullptr, count, 0,
                       height, shade(colour_, 160));
            paintTrace(*gc, haveFrame_ ? left_.data() + traceStart_ : nullptr, count, 0,
                       height, colour_);
            break;
    }
}

void OscilloscopePanel::paintTrace(wxGraphicsContext& gc, const float* samples,
                                   std::size_t count, int top, int height,
                                   const wxColour& colour) {
    const int width = GetClientSize().GetWidth();
    if (width <= 0 || height <= 0) {
        return;
    }
    const double centre    = top + (height / 2.0);
    const double amplitude = std::max(1.0, (height / 2.0) - FromDIP(kPadding));
    const double hairline  = FromDIP(1);

    gc.SetPen(wxPen(withAlpha(colour, kCentreAlpha), hairline));
    gc.StrokeLine(0, centre, width, centre);

    if (samples == nullptr || count == 0) {
        return;
    }

    // One (low, high) a device pixel column. With more samples than columns
    // the two differ and the trace is the band between them -- what a scope's
    // phosphor shows for a signal faster than the sweep -- and with fewer they
    // are the same value, and the band is a line.
    columns_.resize(static_cast<std::size_t>(width));
    foldForDisplay(std::span<const float>{samples, count}, static_cast<float>(gain_),
                   logScale_ ? ScopeScale::Logarithmic : ScopeScale::Linear, columns_);

    const auto y = [&](float value) { return centre - (static_cast<double>(value) * amplitude); };
    const auto x = [](std::size_t column) { return static_cast<double>(column) + 0.5; };

    // The band: across the highs, back along the lows. Filled in the trace
    // colour, so a dense signal is solid where it should be; stroked along
    // both edges, so a sparse one is a line of the chosen width.
    wxGraphicsPath band = gc.CreatePath();
    band.MoveToPoint(x(0), y(columns_[0].second));
    for (std::size_t column = 1; column < columns_.size(); ++column) {
        band.AddLineToPoint(x(column), y(columns_[column].second));
    }
    for (std::size_t column = columns_.size(); column-- > 0;) {
        band.AddLineToPoint(x(column), y(columns_[column].first));
    }
    band.CloseSubpath();

    if (fill_) {
        // Between the trace and the centre line: the highs down to the
        // centre, and the lows up to it, each as its own polygon.
        for (const bool highs : {true, false}) {
            wxGraphicsPath area = gc.CreatePath();
            area.MoveToPoint(x(0), centre);
            for (std::size_t column = 0; column < columns_.size(); ++column) {
                const float value = highs ? columns_[column].second : columns_[column].first;
                area.AddLineToPoint(x(column), y(value));
            }
            area.AddLineToPoint(x(columns_.size() - 1), centre);
            area.CloseSubpath();
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(withAlpha(colour, kFillAlpha)));
            gc.FillPath(area);
        }
    }

    gc.SetBrush(wxBrush(colour));
    gc.SetPen(wxPen(colour, FromDIP(1) * strokeWidth_));
    gc.DrawPath(band);
}

void OscilloscopePanel::onContextMenu(wxContextMenuEvent& event) {
    // The three choices that get flipped while looking at the trace, and a
    // way to the rest. Ticks read from settings, so the menu says what the
    // pane says.
    wxMenu menu;
    menu.AppendRadioItem(kMenuMono, _("Mono"));
    menu.AppendRadioItem(kMenuLeft, _("Left"));
    menu.AppendRadioItem(kMenuRight, _("Right"));
    menu.AppendRadioItem(kMenuStacked, _("Stereo, stacked"));
    menu.AppendRadioItem(kMenuOverlaid, _("Stereo, overlaid"));
    menu.AppendSeparator();
    menu.AppendCheckItem(kMenuTrigger, _("Hold a steady tone still"));
    menu.AppendCheckItem(kMenuFill, _("Fill under the trace"));
    menu.AppendCheckItem(kMenuLogScale, _("Logarithmic scale"));
    menu.AppendSeparator();
    menu.Append(kMenuPreferences, trUtf8("Preferences\xE2\x80\xA6"));

    const Channels channels = channelsFromKey(settings_.ScopeChannels());
    menu.Check(kMenuMono, channels == Channels::Mono);
    menu.Check(kMenuLeft, channels == Channels::Left);
    menu.Check(kMenuRight, channels == Channels::Right);
    menu.Check(kMenuStacked, channels == Channels::Stacked);
    menu.Check(kMenuOverlaid, channels == Channels::Overlaid);
    menu.Check(kMenuTrigger, settings_.ScopeTrigger());
    menu.Check(kMenuFill, settings_.ScopeFill());
    menu.Check(kMenuLogScale, settings_.ScopeLogScale());

    const wxPoint where = event.GetPosition();
    const wxPoint at    = (where == wxDefaultPosition) ? wxPoint(0, 0) : ScreenToClient(where);

    // Synchronously, as the spectrum's: the id comes straight back.
    const int chosen = GetPopupMenuSelectionFromUser(menu, at);
    if (chosen != wxID_NONE) {
        applyMenuItem(chosen);
    }
}

void OscilloscopePanel::applyMenuItem(int item) {
    const auto channels = [this](Channels chosen) {
        settings_.setScopeChannels(channelsKey(chosen));
        settingChanged.publish("scopeChannels");
    };
    switch (item) {
        case kMenuMono:     channels(Channels::Mono); break;
        case kMenuLeft:     channels(Channels::Left); break;
        case kMenuRight:    channels(Channels::Right); break;
        case kMenuStacked:  channels(Channels::Stacked); break;
        case kMenuOverlaid: channels(Channels::Overlaid); break;
        case kMenuTrigger:
            settings_.setScopeTrigger(!settings_.ScopeTrigger());
            settingChanged.publish("scopeTrigger");
            break;
        case kMenuFill:
            settings_.setScopeFill(!settings_.ScopeFill());
            settingChanged.publish("scopeFill");
            break;
        case kMenuLogScale:
            settings_.setScopeLogScale(!settings_.ScopeLogScale());
            settingChanged.publish("scopeLogScale");
            break;
        case kMenuPreferences:
            settingsRequested.publish();
            break;
        default:
            break;
    }
}

}  // namespace xpcog::app
