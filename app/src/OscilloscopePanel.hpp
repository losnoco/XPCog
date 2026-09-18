// The oscilloscope: the live waveform, as a trace.
//
// No Cog counterpart -- Cog has a spectrum and nothing else -- so this is
// written rather than ported, on the same terms as the seek bar's waveform. It
// is the spectrum panel's sibling in every structural respect: the same tap,
// its own TapCursor advanced by the clock rather than by the writer, the same
// visible-and-playing gate on the timer, the same wxBG_STYLE_PAINT and buffered
// DC, and the same absence of a wxEVT_SHOW handler (SpectrumPanel.cpp says why,
// at length). What differs is what is drawn and how much of it the listener can
// change: colour, background, stroke, frame rate, gain, window, fill, trigger
// and channels, all settings, all re-read by applySettings().
//
// The arithmetic -- where a window starts so a tone holds still, how a window
// wider than the screen folds to it -- is in core, in Oscilloscope.hpp, so it
// is tested without a window.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/AudioTap.hpp"

#include <wx/colour.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

class wxContextMenuEvent;
class wxGraphicsContext;

namespace xpcog::app {

class OscilloscopePanel : public wxWindow {
public:
    /// Which channels the trace shows. The `scopeChannels` setting, decoded.
    enum class Channels { Mono, Left, Right, Stacked, Overlaid };

    /// The setting's spelling of each, in the order the menu and the pane list
    /// them, and the reverse: an unknown spelling reads as Mono.
    [[nodiscard]] static const char* channelsKey(Channels channels) noexcept;
    [[nodiscard]] static Channels    channelsFromKey(std::string_view key) noexcept;

    /// The context menu's item ids, public so a test can drive
    /// applyMenuItem() without popping a menu -- nothing can click one under
    /// Xvfb, which has no window manager.
    enum MenuItem : int {
        kMenuMono = 1,
        kMenuLeft,
        kMenuRight,
        kMenuStacked,
        kMenuOverlaid,
        kMenuTrigger,
        kMenuFill,
        kMenuPreferences,
    };

    /// `tap` is borrowed and must outlive this widget, which it does: the
    /// playback controller owns it and, transitively, this window. `settings`
    /// is written by the context menu.
    OscilloscopePanel(wxWindow* parent, AudioTap& tap, Settings& settings);
    ~OscilloscopePanel() override;

    /// What turns a frame interval into frames of audio, and a window in
    /// milliseconds into one in samples.
    void setSampleRate(double rate);

    /// Re-reads every scope setting. One function rather than a setter each,
    /// for the reason SpectrumPanel gives: two readers of the same list must
    /// not disagree about what is on it.
    void applySettings(const Settings& settings);

    /// Starts and stops the repaint clock: visible and playing, or it stops.
    void setActive(bool active);

    /// Does what choosing `item` from the context menu does: writes the
    /// setting and announces it. The menu handler calls this; so does the test.
    void applyMenuItem(int item);

    /// A setting was written here, by the context menu. The owner routes it
    /// through the same path a Preferences change takes, which ends in
    /// applySettings() on this panel -- so the menu and the pane cannot
    /// disagree.
    Signal<std::string> settingChanged;

    /// The context menu's Preferences item. MainFrame opens the Visualizers
    /// pane; this panel knows nothing about that dialog.
    Signal<> settingsRequested;

    [[nodiscard]] Channels channels() const noexcept { return channels_; }

private:
    void onPaint(wxPaintEvent& event);
    void onContextMenu(wxContextMenuEvent& event);
    void tick();

    /// One trace, in a band of the client area: `samples` folded to the
    /// band's width, stroked in `colour`, filled to its centre line if asked.
    void paintTrace(wxGraphicsContext& gc, const float* samples, std::size_t count,
                    int top, int height, const wxColour& colour);

    /// Frames of audio one window covers at the current rate, and how many
    /// the tap can hand over at once (two windows, for the trigger's search,
    /// bounded by what the tap holds behind a paced reader).
    [[nodiscard]] std::size_t windowFrames() const noexcept;
    [[nodiscard]] std::size_t fetchFrames() const noexcept;

    AudioTap& tap_;
    Settings& settings_;
    TapCursor cursor_;
    wxTimer   timer_;

    std::chrono::steady_clock::time_point lastTick_{};
    double                                sampleRate_ = 0.0;

    /// The audio read this frame: the mono lane always (the trigger runs on
    /// it), the sides when a stereo mode wants them. Sized once per settings
    /// change and reused, not allocated per tick.
    std::vector<float> mono_;
    std::vector<float> left_;
    std::vector<float> right_;
    /// Where in those the displayed window starts, and how long it is.
    std::size_t traceStart_  = 0;
    std::size_t traceFrames_ = 0;
    bool        haveFrame_   = false;

    /// Per-column extremes, reused across paints.
    std::vector<std::pair<float, float>> columns_;

    bool playing_ = false;

    // The look, from settings.
    wxColour colour_{"#30d158"};
    wxColour background_{"#121214"};
    double   strokeWidth_ = 1.5;
    int      frameRate_   = 60;
    double   gain_        = 1.0;
    int      windowMs_    = 40;
    bool     fill_        = false;
    bool     trigger_     = true;
    Channels channels_    = Channels::Mono;
};

}  // namespace xpcog::app
