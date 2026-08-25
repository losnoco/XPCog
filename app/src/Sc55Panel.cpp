#include "Sc55Panel.hpp"

#include "Text.hpp"

#include "sc55_resources.hpp"

#include "xpcog/core/audio/PanelFeed.hpp"

#include <api.h>

#include <wx/bitmap.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/translation.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace xpcog::app {
namespace {

/// Thirty a second. The panel is a character LCD whose firmware repaints it far
/// faster than that; what is being chosen here is how often a *person* sees a
/// change, and the feed's own 5 ms floor has already thrown away the rest.
constexpr int kRefreshMs = 33;

/// The emulator writes into a fixed 1024-wide buffer whatever the panel's real
/// size is, so the stride and the visible width are different numbers.
constexpr int kStride = lcd_width_max;

[[nodiscard]] std::vector<std::uint32_t> loadBackground() {
    std::vector<std::uint32_t> pixels;

    const std::span<const std::byte> bytes = resources::sc55("back.data");
    if (bytes.size() != lcd_background_size * sizeof(std::uint32_t)) {
        return pixels;
    }
    pixels.resize(lcd_background_size);
    std::memcpy(pixels.data(), bytes.data(), bytes.size());
    return pixels;
}

}  // namespace

Sc55Panel::Sc55Panel(wxWindow* parent, std::function<double()> position)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      position_(std::move(position)),
      timer_(this) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    background_ = loadBackground();
    buffer_.assign(lcd_buffer_size, 0);
    rgb_.assign(static_cast<std::size_t>(lcd_background_width) * lcd_background_height * 3,
                0);

    SetMinSize(FromDIP(wxSize(lcd_background_width / 3, lcd_background_height / 3)));
    SetSize(FromDIP(wxSize(lcd_background_width / 2, lcd_background_height / 2)));

    Bind(wxEVT_PAINT, &Sc55Panel::onPaint, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
}

Sc55Panel::~Sc55Panel() { timer_.Stop(); }

void Sc55Panel::setActive(bool active) {
    if (active) {
        timer_.Start(kRefreshMs);
        return;
    }
    timer_.Stop();
    haveFrame_ = false;
}

void Sc55Panel::tick() {
    if (background_.empty() || !position_) {
        return;
    }
    // A lookup, not a drain: what did the panel look like at the moment now being
    // heard. See PanelFeed::stateAt().
    const std::optional<PanelFrame> draw = PanelFeed::instance().stateAt(position_());
    if (!draw) {
        // Back to the explanation -- and *back* is the direction that was
        // missing. The feed is emptied on a stop, and a panel that goes on
        // drawing the last frame it was handed is showing a machine that is no
        // longer running.
        //
        // Repainted whether or not that is a change, because the explanation
        // itself can change with no state ever arriving: which of the two is
        // drawn comes from producing(), and the feed it answers about is filled
        // by a thread this one never hears from.
        haveFrame_ = false;
        Refresh(false);
        return;
    }

    if (draw->state.size() != sc55_lcd_state_size()) {
        return;
    }
    sc55_lcd_render_screen(background_.data(), buffer_.data(), draw->state.data(),
                           draw->state.size());
    repack();
    haveFrame_ = true;
    Refresh(false);
}

void Sc55Panel::repack() {
    // The cost Qt did not have. See the header for why the channel order is R, G,
    // B from the low bytes and why alpha is dropped rather than honoured.
    for (int y = 0; y < lcd_background_height; ++y) {
        const std::uint32_t* source =
            buffer_.data() + (static_cast<std::size_t>(y) * kStride);
        unsigned char* destination =
            rgb_.data() + (static_cast<std::size_t>(y) * lcd_background_width * 3);

        for (int x = 0; x < lcd_background_width; ++x) {
            const std::uint32_t pixel = source[x];
            destination[(x * 3) + 0]  = static_cast<unsigned char>(pixel);
            destination[(x * 3) + 1]  = static_cast<unsigned char>(pixel >> 8);
            destination[(x * 3) + 2]  = static_cast<unsigned char>(pixel >> 16);
        }
    }
}

void Sc55Panel::onPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    dc.Clear();

    const wxSize size = GetClientSize();

    if (!haveFrame_) {
        // An empty panel has two completely different causes and they look the
        // same, so it says which. "Nothing has been produced" means the track is
        // not playing on a machine that has a front panel -- the OPL3 has none --
        // and no amount of waiting will change that.
        dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        const wxString message =
            PanelFeed::instance().producing()
                ? trUtf8("Waiting for the panel\xE2\x80\xA6")
                : trUtf8("Nothing is playing on the SC-55.\n\nChoose it under "
                         "Preferences \xE2\x86\x92 MIDI; the OPL3 synthesisers have no "
                    "display.");
        const wxRect box(FromDIP(12), FromDIP(12), size.GetWidth() - FromDIP(24),
                         size.GetHeight() - FromDIP(24));
        dc.DrawLabel(message, box, wxALIGN_CENTER);
        return;
    }

    // Aspect preserved: the panel is a photograph of a real object, and a
    // stretched one looks like a mistake rather than a design.
    // Cast rather than relying on the promotion: the emulator's dimensions are
    // enumerators, and C++20 deprecates arithmetic between an enumeration and a
    // floating-point type.
    const auto panelWidth  = static_cast<double>(int{lcd_background_width});
    const auto panelHeight = static_cast<double>(int{lcd_background_height});

    const double scale = std::min(size.GetWidth() / panelWidth,
                                  size.GetHeight() / panelHeight);
    const int width  = std::max(1, static_cast<int>(panelWidth * scale));
    const int height = std::max(1, static_cast<int>(panelHeight * scale));

    // The trailing `true` is wxImage's static_data flag: the image borrows rgb_
    // rather than copying it or taking ownership, which is what stops a second
    // full-frame copy landing on top of the repack. rgb_ outlives the wxImage,
    // which is why borrowing is safe here.
    wxImage frame(lcd_background_width, lcd_background_height, rgb_.data(), true);
    dc.DrawBitmap(wxBitmap(frame.Scale(width, height, wxIMAGE_QUALITY_BILINEAR)),
                  (size.GetWidth() - width) / 2, (size.GetHeight() - height) / 2, false);
}

}  // namespace xpcog::app
