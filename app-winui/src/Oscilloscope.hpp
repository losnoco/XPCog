#pragma once

// The oscilloscope, drawn with Win2D: app-gtk/src/Visualizers.hpp's
// OscilloscopeView with a CanvasControl where GTK has a GtkDrawingArea, and
// the same trigger, fold, layouts, colours and right-click menu. The spectrum
// beside it (Visualizers.hpp) is the pattern this follows.

#include "FrameTicker.hpp"
#include "Painting.hpp"
#include "WinRT.hpp"

#include "VisualizerChannels.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/AudioTap.hpp"

#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>
#include <winrt/Microsoft.Graphics.Canvas.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::winui {

/// The waveform as it plays: a trace a window long, held still on a steady
/// tone by the trigger, in one of the channel layouts.
class OscilloscopeView {
public:
    using Channels = app::ScopeChannels;

    OscilloscopeView(AudioTap& tap, Settings& settings);
    ~OscilloscopeView();

    OscilloscopeView(const OscilloscopeView&)            = delete;
    OscilloscopeView& operator=(const OscilloscopeView&) = delete;

    [[nodiscard]] mux::UIElement element() const { return canvas_; }

    /// What the window length is counted in; known once a device is open.
    void setSampleRate(double rate);
    void applySettings(const Settings& settings);
    /// Runs while the pane is shown and something plays, and only then.
    void setActive(bool active);

    Signal<std::string> settingChanged;
    /// The menu's Preferences..., for the Visualizers page.
    Signal<> settingsRequested;

    [[nodiscard]] Channels channels() const noexcept { return channels_; }

private:
    void tick();
    void draw(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, float width,
              float height);
    void paintTrace(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, int width,
                    const float* samples, std::size_t count, float top, float height, Colour colour);
    [[nodiscard]] std::size_t windowFrames() const noexcept;
    [[nodiscard]] std::size_t fetchFrames() const noexcept;
    void showMenu(winrt::Windows::Foundation::Point at);

    AudioTap& tap_;
    Settings& settings_;

    winrt::Microsoft::Graphics::Canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    FrameTicker                                                 ticker_{[this] { tick(); }};

    TapCursor                             cursor_;
    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;

    std::vector<float> mono_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::size_t        traceStart_  = 0;
    std::size_t        traceFrames_ = 0;
    bool               haveFrame_   = false;

    std::vector<std::pair<float, float>> columns_;

    bool playing_ = false;

    Colour   colour_      = rgba(0.19F, 0.82F, 0.35F);  // Cog's scopeColor
    Colour   background_  = rgba(0.07F, 0.07F, 0.08F);
    double   strokeWidth_ = 1.5;
    int      frameRate_   = 60;
    double   gain_        = 1.0;
    int      windowMs_    = 40;
    bool     fill_        = false;
    bool     trigger_     = true;
    bool     logScale_    = false;
    Channels channels_    = Channels::Mono;
};

}  // namespace xpcog::winui
