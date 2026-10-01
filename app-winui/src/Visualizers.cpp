#include "Visualizers.hpp"

#include "Translations.hpp"

#include <winrt/Microsoft.Graphics.Canvas.Brushes.h>
#include <winrt/Windows.Foundation.Numerics.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace xpcog::winui {

using app::tr;

namespace {

// GTK's constants, unchanged; see app-gtk/src/Visualizers.cpp for each one's
// reason.
constexpr int    kSpectrumFrameMs   = 16;
constexpr int    kGapDenominator    = 3;
constexpr int    kGridLinesDb[]     = {-10, -20, -30, -40, -50, -60, -70};
constexpr Colour kSpectrumBackground{255, 18, 18, 20};
constexpr float  kGridAlpha         = 22.0F / 255.0F;
constexpr float  kSplitAlpha        = 48.0F / 255.0F;
constexpr int    kFrequencyBarPitch = 5;

const Colour kWhite{255, 255, 255, 255};

/// The channel layouts, as the right-click menu offers them and the settings
/// key spells them.
constexpr std::pair<const char*, const char*> kSpectrumLayouts[] = {
    {"mono", XPCOG_TRANSLATE("Mono")},
    {"left", XPCOG_TRANSLATE("Left")},
    {"right", XPCOG_TRANSLATE("Right")},
    {"mirrored", XPCOG_TRANSLATE("Stereo, mirrored")},
    {"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
    {"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
};

}  // namespace

SpectrumView::SpectrumView(AudioTap& tap, Settings& settings) : tap_(tap), settings_(settings) {
    for (std::vector<float>& window : windows_) {
        window.assign(SpectrumAnalyzer::kWindowFrames, 0.0F);
    }

    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.MinHeight(48);
    canvas_.MinWidth(200);
    // The analyser's dark ground is part of what it looks like in every
    // theme, as in the other players. Inset from the card's edges rather than
    // filling it: a swap chain is not clipped by a parent's corner radius, and
    // square dark corners over a rounded card read as a mistake.
    canvas_.Margin(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));
    canvas_.ClearColor(kSpectrumBackground);
    canvas_.Draw([this](canvas::UI::Xaml::CanvasControl const& sender,
                        canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
             static_cast<float>(sender.ActualHeight()));
    });
    canvas_.SizeChanged([this](auto&&, auto&&) { updateFrequencyBandCount(); });
    canvas_.ContextRequested([this](auto&&, mux::Input::ContextRequestedEventArgs const& args) {
        winrt::Windows::Foundation::Point at{};
        if (!args.TryGetPosition(canvas_, at)) {
            at = {8, 8};
        }
        showMenu(at);
        args.Handled(true);
    });
    // Unloaded with the pane closed: the timer stops with it, as GTK stops
    // its own at unmap.
    canvas_.Unloaded([this](auto&&, auto&&) { timer_.Stop(); });
    canvas_.Loaded([this](auto&&, auto&&) { setActive(playing_); });

    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(kSpectrumFrameMs));
    timer_.Tick([this](auto&&, auto&&) { tick(); });

    applySettings(settings_);
}

SpectrumView::~SpectrumView() {
    // This runs after the XAML loop has ended -- the player is torn down once
    // Application::Start returns -- and by then the dispatcher queue is shut
    // down and a call on it throws. A throw out of a destructor is
    // std::terminate, which a debug build shows as an "abort() has been
    // called" box at every close. Nothing here is worth that: with the loop
    // gone the timer can fire no more ticks anyway.
    try {
        timer_.Stop();
    } catch (const winrt::hresult_error&) {
    }
}

void SpectrumView::setSampleRate(double rate) {
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.prepare(rate);
    }
    cursor_.setSampleRate(rate);
}

void SpectrumView::applySettings(const Settings& settings) {
    if (const auto bar = parseColour(settings.SpectrumBarColor())) {
        barColor_ = *bar;
    }
    if (const auto peak = parseColour(settings.SpectrumDotColor())) {
        peakColor_ = *peak;
    }
    showPeaks_ = settings.SpectrumShowPeaks();

    const Channels channels = app::spectrumChannelsFromKey(settings.SpectrumChannels());
    if (channels != channels_) {
        channels_ = channels;
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
    }
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFloorDb(settings.SpectrumFloorDb());
        analyzer.setMode(settings.SpectrumFreqMode() ? SpectrumAnalyzer::Mode::Frequencies
                                                     : SpectrumAnalyzer::Mode::NoteBands);
    }
    updateFrequencyBandCount();
    canvas_.Invalidate();
}

void SpectrumView::updateFrequencyBandCount() {
    if (analyzers_[0].mode() != SpectrumAnalyzer::Mode::Frequencies) {
        return;
    }
    const auto bars = static_cast<std::size_t>(
        std::max(1, static_cast<int>(canvas_.ActualWidth()) / kFrequencyBarPitch));
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFrequencyBandCount(bars);
    }
}

void SpectrumView::setActive(bool active) {
    playing_ = active;
    if (!active) {
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
        canvas_.Invalidate();
    }
    if (active && canvas_.IsLoaded()) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start();
    } else {
        timer_.Stop();
    }
}

void SpectrumView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    const bool    both  = app::stereo(channels_);
    const TapLane first = (channels_ == Channels::Mono)    ? TapLane::Mono
                          : (channels_ == Channels::Right) ? TapLane::Right
                                                           : TapLane::Left;

    if (!cursor_.read(tap_, elapsed, windows_[0].data(), windows_[0].size(), first)) {
        return;
    }
    if (both && !cursor_.readAgain(tap_, windows_[1].data(), windows_[1].size(), TapLane::Right)) {
        return;
    }
    analyzers_[0].analyze(windows_[0].data(), windows_[0].size());
    if (both) {
        analyzers_[1].analyze(windows_[1].data(), windows_[1].size());
    }
    canvas_.Invalidate();
}

void SpectrumView::draw(const canvas::CanvasDrawingSession& ds, float width, float height) {
    if (analyzers_[0].bands().empty() || width <= 0.0F || height <= 0.0F) {
        return;
    }
    const SpectrumAnalyzer& left  = analyzers_[0];
    const SpectrumAnalyzer& right = analyzers_[1];
    const Colour            split = withAlpha(kWhite, kSplitAlpha);

    switch (channels_) {
        case Channels::Mono:
        case Channels::Left:
        case Channels::Right:
            paintGrid(ds, width, 0.0F, height, false);
            paintBars(ds, width, left, 0.0F, height, false, barColor_, peakColor_);
            break;
        case Channels::Mirrored: {
            const float half = std::floor(height / 2.0F);
            paintGrid(ds, width, 0.0F, half, false);
            paintGrid(ds, width, half, height - half, true);
            paintBars(ds, width, left, 0.0F, half, false, barColor_, peakColor_);
            paintBars(ds, width, right, half, height - half, true, barColor_, peakColor_);
            ds.DrawLine(0.0F, half + 0.5F, width, half + 0.5F, split, 1.0F);
            break;
        }
        case Channels::Stacked: {
            const float half = std::floor(height / 2.0F);
            paintGrid(ds, width, 0.0F, half, false);
            paintGrid(ds, width, half, height - half, false);
            paintBars(ds, width, left, 0.0F, half, false, barColor_, peakColor_);
            paintBars(ds, width, right, half, height - half, false, barColor_, peakColor_);
            ds.DrawLine(0.0F, half + 0.5F, width, half + 0.5F, split, 1.0F);
            break;
        }
        case Channels::Overlaid:
            paintGrid(ds, width, 0.0F, height, false);
            paintBars(ds, width, right, 0.0F, height, false, shade(barColor_, 160),
                      shade(peakColor_, 160));
            paintBars(ds, width, left, 0.0F, height, false, barColor_, peakColor_);
            break;
    }
}

void SpectrumView::paintGrid(const canvas::CanvasDrawingSession& ds, float width, float top,
                             float height, bool flipped) const {
    const double floorDb = analyzers_[0].floorDb();
    const Colour line    = withAlpha(kWhite, kGridAlpha);
    for (const int decibels : kGridLinesDb) {
        if (static_cast<double>(decibels) <= floorDb) {
            continue;
        }
        const auto  level = static_cast<float>((static_cast<double>(decibels) - floorDb) / -floorDb);
        const float y = std::round(flipped ? top + (level * height) : top + height - (level * height)) +
                        0.5F;
        ds.DrawLine(0.0F, y, width, y, line, 1.0F);
    }
}

void SpectrumView::paintBars(const canvas::CanvasDrawingSession& ds, float width,
                             const SpectrumAnalyzer& analyzer, float top, float height,
                             bool flipped, Colour bar, Colour peak) const {
    const std::vector<float>& bands = analyzer.bands();
    const std::vector<float>& peaks = analyzer.peaks();
    if (bands.empty() || height <= 0.0F) {
        return;
    }
    const float slot     = width / static_cast<float>(bands.size());
    const float gap      = slot / kGapDenominator;
    const float barWidth = std::max(1.0F, slot - gap);

    const float base = flipped ? top : top + height;
    const float tip  = flipped ? top + height : top;

    // Darker at the root and lighter at the tip, across the whole pane, so a
    // short bar is dark and a tall one reaches the light: GTK's gradient.
    auto gradient = canvas::Brushes::CanvasLinearGradientBrush(ds, shade(bar, 160), shade(bar, 80));
    gradient.StartPoint({0.0F, base});
    gradient.EndPoint({0.0F, tip});

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const float level = bands[band];
        if (level <= 0.0F) {
            continue;
        }
        const float x      = static_cast<float>(band) * slot;
        const float length = level * height;
        ds.FillRectangle(x, flipped ? base : base - length, barWidth, length, gradient);
    }

    if (!showPeaks_) {
        return;
    }
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const float held = peaks[band];
        if (held <= 0.0F) {
            continue;
        }
        const float x = static_cast<float>(band) * slot;
        const float y = std::round(flipped ? base + (held * height) : base - (held * height)) + 0.5F;
        ds.DrawLine(x, y, x + barWidth, y, peak, 1.0F);
    }
}

void SpectrumView::showMenu(winrt::Windows::Foundation::Point at) {
    auto menu = mux::Controls::MenuFlyout();
    const std::string current = settings_.SpectrumChannels();
    for (const auto& [key, label] : kSpectrumLayouts) {
        auto item = mux::Controls::RadioMenuFlyoutItem();
        item.GroupName(L"layout");
        item.Text(toH(tr(label)));
        item.IsChecked(current == key);
        item.Click([this, key = std::string(key)](auto&&, auto&&) {
            settings_.setSpectrumChannels(key);
            settingChanged.publish("spectrumChannels");
            applySettings(settings_);
        });
        menu.Items().Append(item);
    }
    menu.Items().Append(mux::Controls::MenuFlyoutSeparator());
    auto peaks = mux::Controls::ToggleMenuFlyoutItem();
    peaks.Text(toH(tr("Show peak markers")));
    peaks.IsChecked(settings_.SpectrumShowPeaks());
    peaks.Click([this](auto&&, auto&&) {
        settings_.setSpectrumShowPeaks(!settings_.SpectrumShowPeaks());
        settingChanged.publish("spectrumShowPeaks");
        applySettings(settings_);
    });
    menu.Items().Append(peaks);
    menu.Items().Append(mux::Controls::MenuFlyoutSeparator());
    auto preferences = mux::Controls::MenuFlyoutItem();
    preferences.Text(toH(tr("Preferences\xE2\x80\xA6")));
    preferences.Click([this](auto&&, auto&&) { settingsRequested.publish(); });
    menu.Items().Append(preferences);

    auto options = mux::Controls::Primitives::FlyoutShowOptions();
    options.Position(at);
    menu.ShowAt(canvas_, options);
}

}  // namespace xpcog::winui
