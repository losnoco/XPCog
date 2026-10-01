#pragma once

// The transport's position bar: app-gtk/src/SeekBar.hpp's, in WinUI.
//
// **Plain, it is WinUI's own Slider**, for the reason GTK's is a GtkScale: the
// theme's look and accent, keyboard stepping and an accessible value come with
// it. WinUI's Slider jumps to a click rather than paging towards it, which is
// the behaviour the wx bar is drawn by hand to have. What it lacks is a "let
// go": a drag reports every step through ValueChanged. Pointer handlers
// registered with handledEventsToo see the press and the release the Slider's
// own thumb consumes, so a drag is a scrub, the release is the one seek, and a
// keyboard step -- no press behind it -- seeks at once.
//
// **The waveform is painted** with Win2D, because no stock slider draws a
// track's shape where the groove is. Until a shape is known -- a stream, a
// track the analyser has not reached -- waveform mode shows the plain slider,
// centred in the taller strip, so the bar is never blank.

#include "Painting.hpp"
#include "WinRT.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/Waveform.hpp"

#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>
#include <winrt/Microsoft.Graphics.Canvas.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <memory>
#include <optional>

namespace xpcog::winui {

class SeekBar {
public:
    SeekBar();
    ~SeekBar();

    SeekBar(const SeekBar&)            = delete;
    SeekBar& operator=(const SeekBar&) = delete;

    [[nodiscard]] mux::UIElement element() const { return root_; }

    /// The track's length. Zero disables the bar, which is what a stream with
    /// no known duration wants: nothing to seek within, so nothing to drag.
    void setDuration(double seconds);
    [[nodiscard]] double duration() const noexcept { return duration_; }

    /// Where playback is. Ignored while the listener is dragging, so an update
    /// arriving mid-scrub cannot yank the thumb out from under the pointer.
    void setPosition(double seconds);

    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

    /// Taller, and drawing the track's shape when one has been given. Off is
    /// the plain bar.
    void setWaveformMode(bool on);
    [[nodiscard]] bool waveformMode() const noexcept { return waveformMode_; }

    /// How the shape is drawn: the GTK and wx WaveformStyle, with the two
    /// colours the listener may choose.
    struct WaveformStyle {
        bool                  rectified   = false;
        bool                  logarithmic = false;
        int                   height      = 28;
        std::optional<Colour> played;
        std::optional<Colour> unplayed;
    };
    void setWaveformStyle(const WaveformStyle& style);
    [[nodiscard]] static WaveformStyle styleFrom(const Settings& settings);

    /// The shape to draw, or nothing. Kept whether or not the mode is on, so
    /// turning the mode back on does not have to wait for another.
    void setWaveform(std::shared_ptr<const WaveformSummary> summary);
    [[nodiscard]] const std::shared_ptr<const WaveformSummary>& waveform() const noexcept {
        return waveform_;
    }

    /// The listener let go, or stepped with the keyboard. Carries seconds.
    Signal<double> seekRequested;
    /// The position moved while held, for the clock to follow.
    Signal<double> scrubbed;

private:
    void choose();
    void syncSlider();
    void invalidate();
    void draw(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, float width,
              float height);
    void paintWaveform(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds,
                       float left, float width, float centreY, float halfHeight);
    [[nodiscard]] double positionAt(double x) const;
    [[nodiscard]] float  thumbCentre() const;
    [[nodiscard]] Colour foreground() const;
    [[nodiscard]] Colour accent() const;

    mux::Controls::Grid                                        root_{nullptr};
    mux::Controls::Slider                                      slider_{nullptr};
    winrt::Microsoft::Graphics::Canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    winrt::Windows::UI::ViewManagement::UISettings              uiSettings_;
    winrt::event_token                                          colourToken_{};

    double duration_  = 0.0;
    double position_  = 0.0;
    bool   scrubbing_ = false;
    /// A pointer is down on the slider.
    bool pressed_ = false;
    /// Set while the code moves the slider, so its ValueChanged is not taken
    /// for the listener's.
    bool syncing_ = false;

    bool                                   waveformMode_ = false;
    WaveformStyle                          style_;
    std::shared_ptr<const WaveformSummary> waveform_;

    /// The canvas callbacks and the colour-change handler, which can fire on
    /// another thread, find this object through here; cleared in the
    /// destructor so a late one finds nothing.
    std::shared_ptr<SeekBar*> self_;
};

}  // namespace xpcog::winui
