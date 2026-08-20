// The Roland SC-55's front panel, in step with what is coming out of the
// speakers.
//
// Port of Cog Visualization/SCView.m. The emulator draws the panel itself --
// sc55_lcd_render_screen() composites a state against a photograph of the
// hardware -- so what is left here is when to ask for it and how to get the
// pixels onto a widget.
//
// **When** is the whole difficulty, and it is answered elsewhere: PanelFeed holds
// the states with a position in their track, and this looks one up against the
// position the speaker has actually reached. See PanelFeed.hpp for why that
// cannot work the way the spectrum's synchronisation does.
//
// **How** is where wx costs something Qt did not. The emulator composites into a
// `uint32_t` buffer with a fixed stride of 1024 whatever the panel's real width
// is, and Qt could wrap that buffer as an image with no copy at all -- a
// QImage over a caller-owned pointer, with the stride passed in. wxImage takes
// packed 24-bit RGB and no stride, so each frame is repacked. At thirty frames a
// second over roughly 200,000 pixels that is a real cost where there was none,
// and it is recorded as such in docs/WXPORT.md.
//
// The byte order in that repack is the part to be careful with, and the reason is
// the same one the Qt version's comment gives: `lcd_buffer_t` is `uint32_t*`, and
// on a little-endian machine its bytes come out R, G, B, A. Reading the word as
// 0xAARRGGBB reverses the visible channels and turns the SC-55's amber panel
// blue, which is what Cog shows. Alpha is dropped rather than honoured, because
// the SC-55's two colour constants carry alpha zero -- unlike the JV-880's and
// unlike the background photograph's -- so respecting it would draw the panel and
// leave the characters invisible.

#pragma once

#include <wx/timer.h>
#include <wx/window.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace xpcog::app {

class Sc55Panel : public wxWindow {
public:
    /// `position` reports where the speaker has reached in the current track, in
    /// seconds. A callback rather than a controller reference, because that is the
    /// whole of what this needs to know about playback.
    Sc55Panel(wxWindow* parent, std::function<double()> position);

private:
    void onPaint(wxPaintEvent& event);
    void onShow(wxShowEvent& event);

    /// Looks up the state for the moment now being heard and renders it.
    void tick();

    /// RGBX with a 1024-word stride into packed RGB24, which is the only shape
    /// wxImage takes.
    void repack();

    std::function<double()> position_;
    wxTimer                 timer_;

    /// The photograph the emulator composites onto, and the buffer it composites
    /// into. Both are the emulator's shapes, not ours.
    std::vector<std::uint32_t> background_;
    std::vector<std::uint32_t> buffer_;

    /// The repacked frame, and the bitmap built from it. Held rather than
    /// allocated per paint.
    std::vector<unsigned char> rgb_;
    bool                       haveFrame_ = false;
};

}  // namespace xpcog::app
