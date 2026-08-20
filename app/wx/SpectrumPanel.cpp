#include "SpectrumPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/AudioTap.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace xpcog::app {
namespace {

/// 60 Hz. Cog redraws on a display link; a timer is the portable equivalent and
/// the difference is not visible on a bar chart.
constexpr int kFrameIntervalMs = 16;

/// Cog divides the space between bars by this (bar_gap_denominator = 3): a third
/// of each slot is gap. Reproduced rather than guessed, because it is what makes a
/// spectrum look like Cog's rather than like a solid block.
constexpr int kGapDenominator = 3;

/// The dB gridlines Cog labels, drawn behind the bars.
constexpr int kGridLinesDb[] = {-10, -20, -30, -40, -50, -60, -70};

/// The background. Its own rather than the system's: bars on a light panel in a
/// dark theme, or the reverse, is what inheriting it produces.
const wxColour kBackground{18, 18, 20};

/// Qt's QColor::darker()/lighter(), which wx has no equivalent for. The factor is
/// a percentage as Qt spells it -- 160 is "160% darker", i.e. scaled by 100/160.
[[nodiscard]] wxColour shade(const wxColour& colour, int factor) {
    const auto scale = [factor](unsigned char channel) {
        const int scaled = (static_cast<int>(channel) * 100) / factor;
        return static_cast<unsigned char>(std::clamp(scaled, 0, 255));
    };
    return {scale(colour.Red()), scale(colour.Green()), scale(colour.Blue())};
}

}  // namespace

SpectrumPanel::SpectrumPanel(wxWindow* parent, AudioTap& tap)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      tap_(tap),
      timer_(this),
      window_(SpectrumAnalyzer::kWindowFrames, 0.0F) {
    // Not optional. Without it wx erases the background before the paint handler
    // runs, and the bars flicker at sixty frames a second.
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(120, 48)));
    SetSize(FromDIP(wxSize(400, 120)));

    Bind(wxEVT_PAINT, &SpectrumPanel::onPaint, this);
    Bind(wxEVT_SIZE, &SpectrumPanel::onSize, this);
    Bind(wxEVT_SHOW, &SpectrumPanel::onShow, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
}

void SpectrumPanel::setSampleRate(double rate) { analyzer_.prepare(rate); }

void SpectrumPanel::applySettings(const Settings& settings) {
    // Invalid colour text keeps whatever was there, rather than falling back to a
    // default colour -- which is black, and a black bar on a near-black background
    // is indistinguishable from the spectrum having stopped working. wxColour::Set
    // accepts "#rgb", "#rrggbb" and the SVG colour names, so a hand-edited settings
    // file has a fair chance of meaning what it says.
    if (wxColour bar; bar.Set(toWx(settings.SpectrumBarColor()))) {
        barColor_ = bar;
    }
    if (wxColour peak; peak.Set(toWx(settings.SpectrumDotColor()))) {
        peakColor_ = peak;
    }
    showPeaks_ = settings.SpectrumShowPeaks();

    analyzer_.setFloorDb(settings.SpectrumFloorDb());
    analyzer_.setMode(settings.SpectrumFreqMode()
                          ? SpectrumAnalyzer::Mode::Frequencies
                          : SpectrumAnalyzer::Mode::NoteBands);
    updateFrequencyBandCount();
    Refresh();
}

void SpectrumPanel::updateFrequencyBandCount() {
    if (analyzer_.mode() != SpectrumAnalyzer::Mode::Frequencies) {
        return;
    }
    analyzer_.setFrequencyBandCount(static_cast<std::size_t>(
        std::max(1, GetClientSize().GetWidth() / FromDIP(kFrequencyBarPitch))));
}

void SpectrumPanel::onSize(wxSizeEvent& event) {
    event.Skip();
    // Only matters in Frequencies mode, where the bar count follows the width.
    // NoteBands has a fixed series and ignores it.
    updateFrequencyBandCount();
}

void SpectrumPanel::setActive(bool active) {
    playing_ = active;
    if (!active) {
        analyzer_.reset();
        Refresh();
    }
    // Visible *and* playing, or the clock stops. Either condition alone is a reason
    // not to be running a 4096-point transform sixty times a second.
    if (active && IsShownOnScreen()) {
        timer_.Start(kFrameIntervalMs);
    } else {
        timer_.Stop();
    }
}

void SpectrumPanel::onShow(wxShowEvent& event) {
    event.Skip();
    if (event.IsShown() && playing_) {
        timer_.Start(kFrameIntervalMs);
    } else if (!event.IsShown()) {
        timer_.Stop();
    }
}

void SpectrumPanel::tick() {
    if (!tap_.readLatest(window_.data(), window_.size())) {
        return;  // nothing has played yet
    }
    analyzer_.analyze(window_.data(), window_.size());
    Refresh(false);
}

void SpectrumPanel::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(kBackground));
    dc.Clear();

    const std::vector<float>& bands = analyzer_.bands();
    const std::vector<float>& peaks = analyzer_.peaks();
    if (bands.empty()) {
        return;
    }

    const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) {
        return;
    }

    const wxSize size   = GetClientSize();
    const auto   width  = static_cast<double>(size.GetWidth());
    const auto   height = static_cast<double>(size.GetHeight());

    // The dB grid, behind everything. Positioned by the same normalisation the
    // bars use, so a line labelled -40 dB really is where a -40 dB bar reaches. It
    // follows the *configured* floor rather than a fixed -80, or the grid would
    // quietly start lying the moment anyone changed it.
    const double floorDb = analyzer_.floorDb();
    gc->SetPen(wxPen(wxColour(255, 255, 255, 22)));
    for (const int decibels : kGridLinesDb) {
        if (static_cast<double>(decibels) <= floorDb) {
            continue;  // below the floor, so off the bottom of the display
        }
        const double level = (static_cast<double>(decibels) - floorDb) / -floorDb;
        const double y     = std::round(height - (level * height));
        gc->StrokeLine(0.0, y, width, y);
    }

    const double slot     = width / static_cast<double>(bands.size());
    const double gap      = slot / kGapDenominator;
    const double barWidth = std::max(1.0, slot - gap);

    // Bottom-to-top gradient over the whole height rather than per bar, so a tall
    // bar and a short one agree about what a given height means -- per-bar
    // gradients make every bar look equally loud at its own tip.
    //
    // Derived from the chosen bar colour rather than a fixed ramp: the colour is a
    // setting, so anything hard-coded here would ignore it. Darker at the bottom
    // and lighter at the top keeps the shape readable without inventing hues
    // nobody picked.
    gc->SetBrush(gc->CreateLinearGradientBrush(0.0, height, 0.0, 0.0,
                                               shade(barColor_, 160),
                                               shade(barColor_, 80)));
    gc->SetPen(*wxTRANSPARENT_PEN);

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const double level = static_cast<double>(bands[band]);
        if (level <= 0.0) {
            continue;
        }
        const double x   = static_cast<double>(band) * slot;
        const double top = height - (level * height);
        gc->DrawRectangle(x, top, barWidth, height - top);
    }

    // The peak markers last, over the bars. Cog draws these as a one-pixel line in
    // its own colour -- spectrumDotColor -- and so does this.
    if (!showPeaks_) {
        return;
    }
    gc->SetPen(wxPen(peakColor_));
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const double peak = static_cast<double>(peaks[band]);
        if (peak <= 0.0) {
            continue;
        }
        const double x = static_cast<double>(band) * slot;
        const double y = std::round(height - (peak * height));
        gc->StrokeLine(x, y, x + barWidth, y);
    }
}

}  // namespace xpcog::app
