// The VU meter, the Waveform pane and the Signal Inspector: app-gtk/src/Meters.hpp
// drawn with Win2D, over the same LevelMeter, EnvelopeHistory and SignalChain.
// Both views take the oscilloscope's colours, so a dark strip stays one strip.

#pragma once

#include "FrameTicker.hpp"
#include "Painting.hpp"
#include "Session.hpp"
#include "WinRT.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/EnvelopeHistory.hpp"
#include "xpcog/core/audio/LevelMeter.hpp"

#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>
#include <winrt/Microsoft.Graphics.Canvas.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::winui {

/// Left over right, peak over RMS, with the peak held and a lamp for clipping.
class VuMeterView {
public:
    VuMeterView(AudioTap& tap, const Settings& settings);

    [[nodiscard]] mux::UIElement element() const { return canvas_; }

    void setSampleRate(double rate) { sampleRate_ = rate; }

    /// Something is playing, and the pane is shown. Off, the meters fall to rest
    /// and the clock stops.
    void setActive(bool active);

private:
    void tick();
    void draw(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, float width,
              float height);

    AudioTap& tap_;
    Colour    colour_{};

    winrt::Microsoft::Graphics::Canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    FrameTicker                                                 ticker_{[this] { tick(); }};

    LevelMeter                            meter_;
    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;
    bool                                  active_     = false;
};

/// The last four seconds, scrolling left.
class EnvelopeView {
public:
    EnvelopeView(AudioTap& tap, const Settings& settings);

    [[nodiscard]] mux::UIElement element() const { return canvas_; }

    void setSampleRate(double rate);
    void setActive(bool active);

private:
    void tick();
    void draw(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, float width,
              float height);

    AudioTap& tap_;
    Colour    colour_{};

    winrt::Microsoft::Graphics::Canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    FrameTicker                                                 ticker_{[this] { tick(); }};

    EnvelopeHistory                       history_;
    TapCursor                             cursor_;
    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;
    bool                                  active_     = false;
    std::vector<float>                    scratch_;
};

/// The path from decoder to device, and the levels at the end of it, in a window
/// of its own. Closing hides it; it reads the engine only while it is shown.
class SignalInspector {
public:
    explicit SignalInspector(app::Session& session);
    ~SignalInspector();

    SignalInspector(const SignalInspector&)            = delete;
    SignalInspector& operator=(const SignalInspector&) = delete;

    void toggle();
    [[nodiscard]] bool shown() const noexcept { return shown_; }

    /// The window closed or opened, for the menu's tick.
    std::function<void()> visibilityChanged;

private:
    void show();
    void hide();
    void refresh();

    app::Session& session_;

    mux::Window                                   window_{nullptr};
    mux::Controls::TextBlock                      verdict_{nullptr};
    mux::Controls::StackPanel                     stages_{nullptr};
    mux::Controls::TextBlock                      live_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    std::unique_ptr<VuMeterView>                  meter_;

    std::string shownStages_;
    bool        shown_   = false;
    bool        closing_ = false;
};

}  // namespace xpcog::winui
