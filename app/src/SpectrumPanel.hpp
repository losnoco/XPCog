// The spectrum display. Replaces Cog's SpectrumViewCG.
//
// Cog ships two of these: SpectrumViewSK draws through SceneKit and SpectrumViewCG
// through Core Graphics. Having a 2D CPU path is therefore Cog's own arrangement,
// not a compromise, and it is the one ported here -- with wxGraphicsContext
// standing in for Core Graphics, which is very nearly a one-for-one substitution.
//
// Where the numbers come from is SpectrumAnalyzer, which is Cog's configuration of
// deadbeef's analyser. This class does nothing but draw them and run the clock.
//
// Two wx-specific requirements that are not optional, both of which produce a
// visibly broken display if forgotten. wxBG_STYLE_PAINT stops the toolkit erasing
// the background before the paint handler runs -- without it the bars flicker at
// sixty frames a second. And wxAutoBufferedPaintDC is what supplies the
// double-buffering; wx does not do it for you on MSW, where Qt always did.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
// For TapCursor, which is held by value. The tap itself is still only borrowed.
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <wx/colour.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <chrono>
#include <vector>

class wxContextMenuEvent;

namespace xpcog::app {

class SpectrumPanel : public wxWindow {
public:
    /// `tap` is borrowed and must outlive this widget, which it does: the playback
    /// controller owns both it and, transitively, this window.
    SpectrumPanel(wxWindow* parent, AudioTap& tap);

    /// Stops the clock. Explicit, because a timer that outlives the window it
    /// draws into is a callback into freed memory, and ~wxTimer running as a
    /// member is late enough to be worth not relying on.
    ~SpectrumPanel() override;

    /// The rate the analysis window is taken at. Needed because the band table
    /// depends on it, and because it is what turns a frame interval into a number
    /// of samples to advance the read cursor by -- the widget has no other way to
    /// learn either.
    void setSampleRate(double rate);

    /// Re-reads every spectrum setting: colours, band mode, floor, peak markers.
    ///
    /// One function rather than a setter each, because these are read from exactly
    /// two places -- construction and a change in Preferences -- and the failure
    /// worth avoiding is those two disagreeing about which settings exist.
    void applySettings(const Settings& settings);

    /// Starts and stops the repaint clock. Called when playback starts or stops,
    /// and when the panel is shown or hidden -- an invisible widget must not be
    /// running a 4096-point FFT sixty times a second.
    void setActive(bool active);

    /// The context menu's one item was chosen. MainFrame opens Preferences on
    /// the Spectrum pane; the panel itself knows nothing about that dialog.
    Signal<> settingsRequested;

private:
    void onPaint(wxPaintEvent& event);
    void onContextMenu(wxContextMenuEvent& event);
    void onSize(wxSizeEvent& event);
    void tick();

    /// Derives the bar count from the widget's width. Frequencies mode only.
    void updateFrequencyBandCount();

    AudioTap&        tap_;

    /// Where in the tap this display has got to.
    ///
    /// The reason it is not simply "the newest window every repaint" is that the
    /// playback chain does not hand the tap audio at anything like this widget's
    /// frame rate. A device period of a few thousand frames arrives every eighty
    /// or ninety milliseconds; five repaints in a row then see the same samples
    /// and the sixth jumps a tenth of a second, which is a display running at
    /// 60 Hz and moving at 11. This advances by the measured frame interval
    /// instead, so the window slides at the rate the audio is being heard at
    /// whatever size the chunks arriving behind it are. See TapCursor.
    TapCursor        cursor_;
    SpectrumAnalyzer analyzer_;
    wxTimer          timer_;

    /// When the last frame was drawn, for the interval the cursor advances by.
    ///
    /// Measured rather than assumed to be kFrameIntervalMs: a wxTimer is not a
    /// display link and does not pretend to be, so the interval it actually
    /// delivers wanders with the load on the UI thread. Advancing by the nominal
    /// figure would make the spectrum drift steadily away from the music, which is
    /// the fault this cursor exists to fix, arrived at from the other side.
    std::chrono::steady_clock::time_point lastTick_{};

    /// The window handed to the analyser each frame. Held rather than allocated
    /// per tick: 4096 floats, sixty times a second, is a pointless amount of churn
    /// to hand the allocator.
    std::vector<float> window_;

    /// Whether playback is running. Separate from the timer, because the timer
    /// also stops when the widget is hidden and the two reasons must not be
    /// confused.
    bool playing_ = false;

    /// Cog's spectrumBarColor and spectrumDotColor, and its defaults for them.
    wxColour barColor_{"#ff8000"};
    wxColour peakColor_{"#ff3b30"};
    bool     showPeaks_ = true;

    /// Pixels per bar when the bands are evenly spaced.
    ///
    /// Frequencies mode only, where the count is ours to choose. Cog picks one band
    /// per pixel column, which at any real window width is finer than the eye
    /// resolves; this aims for a bar every few pixels and hands the resulting count
    /// to the analyser.
    static constexpr int kFrequencyBarPitch = 5;
};

}  // namespace xpcog::app
