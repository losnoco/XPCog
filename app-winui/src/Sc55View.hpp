// The Roland SC-55's front panel, in step with what is coming out of the
// speakers. The counterpart of app-gtk/src/Sc55View.hpp; PanelFeed decides
// *when*, by holding states with a position in their track and answering for
// the position the speaker has reached, and this decides how the pixels get
// onto the screen -- through a Win2D CanvasBitmap over a CanvasControl.
//
// The emulator composites into a buffer 1024 words wide whatever the panel's
// real width is, and writes R, G, B into the low bytes of each word: in memory
// on a little-endian machine R, G, B, then a byte it calls alpha and that
// this ignores. That is DXGI's R8G8B8A8 with the alpha left out of it, which
// is how the bitmap is created. CreateFromBytes and SetPixelBytes take rows
// packed tight, so the visible rows are copied out of the strided buffer each
// frame -- a memcpy per row, no repack.

#pragma once

#ifdef XPCOG_HAVE_SC55_PANEL

#include "WinRT.hpp"

#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>
#include <winrt/Microsoft.Graphics.Canvas.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace xpcog::winui {

class Sc55View {
public:
    /// `position` reports where the speaker has reached in the current track,
    /// in seconds. A callback rather than a controller reference, because that
    /// is the whole of what this needs to know about playback.
    explicit Sc55View(std::function<double()> position);
    ~Sc55View();

    Sc55View(const Sc55View&)            = delete;
    Sc55View& operator=(const Sc55View&) = delete;

    [[nodiscard]] mux::UIElement element() const { return root_; }

    /// Starts and stops the refresh clock. Nothing is switched on in the
    /// *feed* here: states are recorded from the start of the track
    /// regardless, which is what lets this show the right one immediately.
    void setActive(bool active);

private:
    void tick();
    void showExplanation();
    void showPanel();
    void createBitmap(const winrt::Microsoft::Graphics::Canvas::ICanvasResourceCreator& creator);
    void draw(const winrt::Microsoft::Graphics::Canvas::CanvasDrawingSession& ds, float width,
              float height) const;

    std::function<double()> position_;
    bool                    active_ = false;

    mux::Controls::Grid                                    root_{nullptr};
    winrt::Microsoft::Graphics::Canvas::UI::Xaml::CanvasControl canvas_{nullptr};
    mux::Controls::TextBlock                               message_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    /// Made in the control's CreateResources, and again whenever the device
    /// is lost and Win2D asks for its resources a second time.
    winrt::Microsoft::Graphics::Canvas::CanvasBitmap      bitmap_{nullptr};

    /// The photograph the emulator composites onto, and the buffer it
    /// composites into. Both are the emulator's shapes, not ours.
    std::vector<std::uint32_t> background_;
    std::vector<std::uint32_t> buffer_;
    /// The visible rows of the last frame, packed for the bitmap.
    std::vector<std::uint8_t> frame_;
    bool                      haveFrame_ = false;
};

}  // namespace xpcog::winui

#endif  // XPCOG_HAVE_SC55_PANEL
