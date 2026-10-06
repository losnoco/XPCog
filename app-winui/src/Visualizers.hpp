#pragma once

// The painted visualisers, drawn with Win2D: app-gtk/src/Visualizers.hpp's
// views with a CanvasControl where GTK has a GtkDrawingArea, and the same
// analysis, colours, layouts and right-click menus.

#include "FrameTicker.hpp"
#include "Painting.hpp"
#include "WinRT.hpp"

#include "VisualizerChannels.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>
#include <winrt/Microsoft.Graphics.Canvas.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace xpcog::winui {

namespace canvas = winrt::Microsoft::Graphics::Canvas;

/// The analyser: bars per band, peak markers, in one of the channel layouts.
class SpectrumView {
public:
    using Channels = app::SpectrumChannels;

    SpectrumView(AudioTap& tap, Settings& settings);
    ~SpectrumView();

    SpectrumView(const SpectrumView&)            = delete;
    SpectrumView& operator=(const SpectrumView&) = delete;

    [[nodiscard]] mux::UIElement element() const { return canvas_; }

    /// What the band table is built against; known once a device is open.
    void setSampleRate(double rate);
    void applySettings(const Settings& settings);
    /// Runs while the pane is shown and something plays, and only then: a
    /// stopped analyser is a 60 Hz redraw of nothing.
    void setActive(bool active);

    Signal<std::string> settingChanged;
    /// The menu's Preferences..., for the Visualizers page.
    Signal<> settingsRequested;

private:
    void tick();
    void draw(const canvas::CanvasDrawingSession& ds, float width, float height);
    void paintGrid(const canvas::CanvasDrawingSession& ds, float width, float top, float height,
                   bool flipped) const;
    void paintBars(const canvas::CanvasDrawingSession& ds, float width,
                   const SpectrumAnalyzer& analyzer, float top, float height, bool flipped,
                   Colour bar, Colour peak) const;
    void updateFrequencyBandCount();
    void showMenu(winrt::Windows::Foundation::Point at);

    AudioTap& tap_;
    Settings& settings_;

    canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    FrameTicker                     ticker_{[this] { tick(); }};

    TapCursor                             cursor_;
    std::array<SpectrumAnalyzer, 2>       analyzers_;
    std::array<std::vector<float>, 2>     windows_;
    std::chrono::steady_clock::time_point lastTick_{};
    bool                                  playing_ = false;

    Colour   barColor_  = rgba(1.0F, 0.5F, 0.0F);     // Cog's spectrumBarColor
    Colour   peakColor_ = rgba(1.0F, 0.23F, 0.19F);   // and spectrumDotColor
    bool     showPeaks_ = true;
    Channels channels_  = Channels::Mono;
};

}  // namespace xpcog::winui
