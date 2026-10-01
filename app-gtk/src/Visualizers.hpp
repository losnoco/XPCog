// The spectrum and the oscilloscope, painted. Counterparts of
// app-winui/src/Visualizers.hpp and app-winui/src/Oscilloscope.hpp, over the same
// core: SpectrumAnalyzer for the bands, TapCursor for a window that slides at
// the rate the audio is heard at, Oscilloscope.hpp for the trigger and the
// fold. What is here is when to read, and how to get the numbers onto a
// GtkDrawingArea with cairo -- which is very nearly wxGraphicsContext with the
// method names changed, and is why the drawing reads as a transcription.
//
// The clock is a timeout at the frame interval rather than a frame-clock tick
// callback: the wx panels advance the tap cursor by the interval that was
// *measured*, so a late frame costs a frame rather than putting the display
// out of step with the music, and a timeout is the source that measures that
// way. Visible and playing, or it stops; a hidden pane must not be running a
// 4096-point transform sixty times a second for nobody.
//
// Each has the context menu its wx panel has -- the channels, the flags that
// get flipped while looking at the thing, and Preferences... -- as a
// GtkPopoverMenu over an action group on the widget, so the ticks are the
// actions' states and read from settings when the menu opens.

#pragma once

#include "Glib.hpp"
#include "VisualizerChannels.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <gtk/gtk.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::gtk {

/// The right-click menu the two views share the shape of: a popover over a
/// GMenu, with an action group the view fills, parented to the area and
/// unparented before it.
class VisualizerMenu {
public:
    VisualizerMenu(GtkWidget* area, const char* prefix);
    ~VisualizerMenu();

    VisualizerMenu(const VisualizerMenu&)            = delete;
    VisualizerMenu& operator=(const VisualizerMenu&) = delete;

    [[nodiscard]] GSimpleActionGroup* actions() const { return group_.get(); }
    [[nodiscard]] GMenu*              model() const { return menu_.get(); }

    /// Builds the popover from the model; call once the model is filled.
    void build();

    /// Pops the menu at `x`,`y` in the area's coordinates. `sync` runs first,
    /// so the states shown are the settings' at that moment.
    void popup(double x, double y);

    /// Runs before every popup.
    std::function<void()> sync;

private:
    GtkWidget*                     area_;
    GObjectPtr<GSimpleActionGroup> group_;
    GObjectPtr<GMenu>              menu_;
    GtkWidget*                     popover_ = nullptr;
    std::vector<Connection>        connections_;
};

class SpectrumView {
public:
    using Channels = app::SpectrumChannels;

    /// `tap` is borrowed and must outlive this widget, which it does: the
    /// playback controller owns it and the session owns that. `settings` is
    /// read by applySettings() and written by the menu.
    SpectrumView(AudioTap& tap, Settings& settings);
    ~SpectrumView();

    SpectrumView(const SpectrumView&)            = delete;
    SpectrumView& operator=(const SpectrumView&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return area_; }

    /// The rate the analysis window is taken at; the band table depends on it.
    void setSampleRate(double rate);

    /// Re-reads every spectrum setting.
    void applySettings(const Settings& settings);

    /// Starts and stops the repaint clock: visible and playing, or it stops.
    void setActive(bool active);

    /// A setting was written here, by the menu. Carries the key.
    Signal<std::string> settingChanged;
    /// The menu's Preferences item.
    Signal<> settingsRequested;

    [[nodiscard]] Channels channels() const noexcept { return channels_; }

private:
    void tick();
    void draw(cairo_t* cr, int width, int height);
    void paintGrid(cairo_t* cr, double width, double top, double height, bool flipped);
    void paintBars(cairo_t* cr, double width, const SpectrumAnalyzer& analyzer, double top,
                   double height, bool flipped, const GdkRGBA& bar, const GdkRGBA& peak);
    void updateFrequencyBandCount();
    void buildMenu();

    AudioTap& tap_;
    Settings& settings_;

    GObjectPtr<GtkWidget>   owned_;  // see SeekBar
    GtkWidget*              area_ = nullptr;
    std::vector<Connection> connections_;
    VisualizerMenu          menu_;

    TapCursor                             cursor_;
    std::array<SpectrumAnalyzer, 2>       analyzers_;
    Timeout                               timer_;
    std::chrono::steady_clock::time_point lastTick_{};
    std::array<std::vector<float>, 2>     windows_;
    bool                                  playing_ = false;

    GdkRGBA  barColor_{1.0F, 0.5F, 0.0F, 1.0F};   // Cog's spectrumBarColor
    GdkRGBA  peakColor_{1.0F, 0.23F, 0.19F, 1.0F};  // and spectrumDotColor
    bool     showPeaks_ = true;
    Channels channels_  = Channels::Mono;
};

class OscilloscopeView {
public:
    using Channels = app::ScopeChannels;

    OscilloscopeView(AudioTap& tap, Settings& settings);
    ~OscilloscopeView();

    OscilloscopeView(const OscilloscopeView&)            = delete;
    OscilloscopeView& operator=(const OscilloscopeView&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return area_; }

    void setSampleRate(double rate);
    void applySettings(const Settings& settings);
    void setActive(bool active);

    Signal<std::string> settingChanged;
    Signal<>            settingsRequested;

    [[nodiscard]] Channels channels() const noexcept { return channels_; }

private:
    void tick();
    void draw(cairo_t* cr, int width, int height);
    void paintTrace(cairo_t* cr, int width, const float* samples, std::size_t count, int top,
                    int height, const GdkRGBA& colour);
    [[nodiscard]] std::size_t windowFrames() const noexcept;
    [[nodiscard]] std::size_t fetchFrames() const noexcept;
    void                      buildMenu();

    AudioTap& tap_;
    Settings& settings_;

    GObjectPtr<GtkWidget>   owned_;
    GtkWidget*              area_ = nullptr;
    std::vector<Connection> connections_;
    VisualizerMenu          menu_;

    TapCursor                             cursor_;
    Timeout                               timer_;
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

    GdkRGBA  colour_{0.19F, 0.82F, 0.35F, 1.0F};
    GdkRGBA  background_{0.07F, 0.07F, 0.08F, 1.0F};
    double   strokeWidth_ = 1.5;
    int      frameRate_   = 60;
    double   gain_        = 1.0;
    int      windowMs_    = 40;
    bool     fill_        = false;
    bool     trigger_     = true;
    bool     logScale_    = false;
    Channels channels_    = Channels::Mono;
};

}  // namespace xpcog::gtk
