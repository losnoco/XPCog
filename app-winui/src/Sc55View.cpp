#include "Sc55View.hpp"

#ifdef XPCOG_HAVE_SC55_PANEL

#include "Translations.hpp"
#include "sc55_resources.hpp"

#include "xpcog/core/audio/PanelFeed.hpp"

#include <api.h>

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace xpcog::winui {
namespace {

namespace canvas = winrt::Microsoft::Graphics::Canvas;

/// Thirty a second; app/src/Sc55Panel.cpp says why not more.
constexpr int kRefreshMs = 33;

/// The emulator writes into a fixed 1024-wide buffer whatever the panel's
/// real size is, so the stride and the visible width are different numbers.
constexpr int kStride = lcd_width_max;

[[nodiscard]] std::vector<std::uint32_t> loadBackground() {
    std::vector<std::uint32_t> pixels;
    // Compiled in, as the wx player has it: 776 KiB of raw RGBA that is part
    // of the emulator the way its ROMs are not.
    const std::span<const std::byte> bytes = resources::sc55("back.data");
    if (bytes.size() != static_cast<std::size_t>(lcd_background_size) * sizeof(std::uint32_t)) {
        return pixels;
    }
    pixels.resize(lcd_background_size);
    std::memcpy(pixels.data(), bytes.data(), bytes.size());
    return pixels;
}

}  // namespace

Sc55View::Sc55View(std::function<double()> position) : position_(std::move(position)) {
    background_ = loadBackground();
    buffer_.assign(lcd_buffer_size, 0);
    frame_.assign(static_cast<std::size_t>(lcd_background_width) * lcd_background_height * 4, 0);

    root_ = mux::Controls::Grid();
    root_.MinWidth(lcd_background_width / 3.0);
    root_.MinHeight(lcd_background_height / 3.0);
    root_.Margin(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));

    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.CreateResources([this](canvas::UI::Xaml::CanvasControl const& sender, auto&&) {
        createBitmap(sender);
    });
    canvas_.Draw([this](canvas::UI::Xaml::CanvasControl const& sender,
                        canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
             static_cast<float>(sender.ActualHeight()));
    });
    // The clock follows the control on and off the screen; the owner reports
    // whether the section is wanted and this reports whether it is seen --
    // GTK's map and unmap.
    canvas_.Loaded([this](auto&&, auto&&) { setActive(active_); });
    canvas_.Unloaded([this](auto&&, auto&&) { timer_.Stop(); });
    root_.Children().Append(canvas_);

    message_ = secondaryText(L"TextWrapping='Wrap' TextAlignment='Center' MaxWidth='360'"
                             L" HorizontalAlignment='Center' VerticalAlignment='Center'");
    root_.Children().Append(message_);

    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(kRefreshMs));
    timer_.Tick([this](auto&&, auto&&) { tick(); });

    showExplanation();
}

Sc55View::~Sc55View() {
    // Runs after the XAML loop has ended, with the dispatcher queue shut
    // down: a call on it throws, and a throw out of a destructor is
    // std::terminate. With the loop gone the timer can fire no more anyway.
    try {
        timer_.Stop();
    } catch (const winrt::hresult_error&) {
    }
}

void Sc55View::setActive(bool active) {
    active_ = active;
    if (active && canvas_.IsLoaded()) {
        timer_.Start();
        return;
    }
    timer_.Stop();
    showExplanation();
}

void Sc55View::createBitmap(const canvas::ICanvasResourceCreator& creator) {
    // Alpha ignored: the emulator's fourth byte is not an alpha channel, and
    // read as one it would leave the panel see-through.
    bitmap_ = canvas::CanvasBitmap::CreateFromBytes(
        creator, frame_, lcd_background_width, lcd_background_height,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R8G8B8A8UIntNormalized, 96.0F,
        canvas::CanvasAlphaMode::Ignore);
}

void Sc55View::showExplanation() {
    // An empty panel has two completely different causes and they look the
    // same, so it says which. "Nothing has been produced" means the track is
    // not playing on a machine that has a front panel -- the OPL3 has none --
    // and no amount of waiting will change that.
    const std::string text =
        PanelFeed::instance().producing()
            ? app::tr("Waiting for the panel\xE2\x80\xA6")
            : app::tr("Nothing is playing on the SC-55.\n\nChoose it under Preferences "
                      "\xE2\x86\x92 MIDI; the OPL3 synthesisers have no display.");
    message_.Text(toH(text));
    message_.Visibility(mux::Visibility::Visible);
    canvas_.Visibility(mux::Visibility::Collapsed);
    haveFrame_ = false;
}

void Sc55View::showPanel() {
    message_.Visibility(mux::Visibility::Collapsed);
    canvas_.Visibility(mux::Visibility::Visible);
}

void Sc55View::tick() {
    if (background_.empty() || !position_) {
        return;
    }
    // A lookup, not a drain: what did the panel look like at the moment now
    // being heard. See PanelFeed::stateAt().
    const std::optional<PanelFrame> state = PanelFeed::instance().stateAt(position_());
    if (!state) {
        // Back to the explanation: a panel that goes on drawing the last frame
        // it was handed is showing a machine that is no longer running.
        showExplanation();
        return;
    }
    if (state->state.size() != sc55_lcd_state_size()) {
        return;
    }
    sc55_lcd_render_screen(background_.data(), buffer_.data(), state->state.data(),
                           state->state.size());

    // The visible rows, packed: the bitmap has no stride of its own.
    const std::size_t row = static_cast<std::size_t>(lcd_background_width) * sizeof(std::uint32_t);
    for (int y = 0; y < lcd_background_height; ++y) {
        std::memcpy(frame_.data() + (static_cast<std::size_t>(y) * row),
                    buffer_.data() + (static_cast<std::size_t>(y) * kStride), row);
    }
    if (bitmap_) {
        bitmap_.SetPixelBytes(frame_);
    }
    haveFrame_ = true;
    showPanel();
    canvas_.Invalidate();
}

void Sc55View::draw(const canvas::CanvasDrawingSession& ds, float width, float height) const {
    if (!bitmap_ || !haveFrame_ || width <= 0.0F || height <= 0.0F) {
        return;
    }
    // Aspect preserved and centred: the panel is a photograph of a real
    // object, and a stretched one looks like a mistake rather than a design.
    const auto  imageWidth  = static_cast<float>(lcd_background_width);
    const auto  imageHeight = static_cast<float>(lcd_background_height);
    const float scale       = std::min(width / imageWidth, height / imageHeight);
    const float drawnWidth  = imageWidth * scale;
    const float drawnHeight = imageHeight * scale;
    const winrt::Windows::Foundation::Rect destination{(width - drawnWidth) / 2.0F,
                                                       (height - drawnHeight) / 2.0F, drawnWidth,
                                                       drawnHeight};
    const winrt::Windows::Foundation::Rect source{0.0F, 0.0F, imageWidth, imageHeight};
    ds.DrawImage(bitmap_, destination, source);
}

}  // namespace xpcog::winui

#endif  // XPCOG_HAVE_SC55_PANEL
