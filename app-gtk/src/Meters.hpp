// The VU meter, the Waveform pane and the Signal Inspector. Counterparts of
// app-winui/src/Meters.hpp, over the same core: LevelMeter, EnvelopeHistory and
// uicore's SignalChain. Clocked as Visualizers.hpp's views are, by a timeout at
// the frame interval that measures how long it really was.

#pragma once

#include "Glib.hpp"
#include "Session.hpp"

#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/EnvelopeHistory.hpp"
#include "xpcog/core/audio/LevelMeter.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <chrono>
#include <string>
#include <vector>

namespace xpcog::gtk {

/// Left and right, peak over RMS, with the peak held and a lamp for clipping.
class VuMeterView {
public:
    explicit VuMeterView(AudioTap& tap);
    ~VuMeterView();

    VuMeterView(const VuMeterView&)            = delete;
    VuMeterView& operator=(const VuMeterView&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return area_; }

    void setSampleRate(double rate) { sampleRate_ = rate; }

    /// Something is playing, and the pane is shown. Off, the meters fall to rest
    /// and the clock stops.
    void setActive(bool active);

private:
    void tick();
    void draw(cairo_t* cr, int width, int height);

    AudioTap& tap_;

    GObjectPtr<GtkWidget>   owned_;
    GtkWidget*              area_ = nullptr;
    std::vector<Connection> connections_;

    LevelMeter                            meter_;
    Timeout                               timer_;
    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;
    bool                                  active_     = false;
};

/// The last four seconds, scrolling left.
class EnvelopeView {
public:
    explicit EnvelopeView(AudioTap& tap);
    ~EnvelopeView();

    EnvelopeView(const EnvelopeView&)            = delete;
    EnvelopeView& operator=(const EnvelopeView&) = delete;

    [[nodiscard]] GtkWidget* widget() const { return area_; }

    void setSampleRate(double rate);
    void setActive(bool active);

private:
    void tick();
    void draw(cairo_t* cr, int width, int height);

    AudioTap& tap_;

    GObjectPtr<GtkWidget>   owned_;
    GtkWidget*              area_ = nullptr;
    std::vector<Connection> connections_;

    EnvelopeHistory                       history_;
    TapCursor                             cursor_;
    Timeout                               timer_;
    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;
    bool                                  active_     = false;
    std::vector<float>                    scratch_;
};

/// The path from decoder to device, and the levels at the end of it, in a window
/// of its own. Reads the engine only while it is on screen.
class SignalInspector {
public:
    SignalInspector(GtkWindow* parent, app::Session& session);
    ~SignalInspector();

    SignalInspector(const SignalInspector&)            = delete;
    SignalInspector& operator=(const SignalInspector&) = delete;

    void toggle();
    [[nodiscard]] bool shown() const;

    /// The window closed or opened, for the menu's tick.
    std::function<void()> visibilityChanged;

    /// Reads the engine now, rather than on the next tick.
    void refresh();

    [[nodiscard]] GtkWindow* window() const { return GTK_WINDOW(window_); }

private:
    app::Session& session_;

    GtkWidget* window_  = nullptr;
    GtkWidget* verdict_ = nullptr;
    GtkWidget* stages_  = nullptr;
    GtkWidget* live_    = nullptr;

    std::unique_ptr<VuMeterView> meter_;
    std::vector<Connection>      connections_;
    Timeout                      timer_;
    std::string                  shownStages_;
};

}  // namespace xpcog::gtk
