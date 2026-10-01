#include "Sc55View.hpp"

#include "Translations.hpp"

#include "xpcog/core/audio/PanelFeed.hpp"

#include <api.h>

#include <cstring>
#include <optional>
#include <utility>

namespace xpcog::gtk {
namespace {

/// Thirty a second; Sc55Panel.cpp says why not more.
constexpr unsigned kRefreshMs = 33;

/// The emulator writes into a fixed 1024-wide buffer whatever the panel's
/// real size is, so the stride and the visible width are different numbers.
constexpr int kStride = lcd_width_max;

[[nodiscard]] std::vector<std::uint32_t> loadBackground() {
    std::vector<std::uint32_t> pixels;
    // From the resource bundle, beside the interface files: 776 KiB of raw
    // RGBA that is part of the emulator the way its ROMs are not.
    GBytes* bytes = g_resources_lookup_data("/co/losno/XPCog/sc55/back.data",
                                            G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (bytes == nullptr) {
        return pixels;
    }
    gsize       size = 0;
    const void* data = g_bytes_get_data(bytes, &size);
    if (size == static_cast<gsize>(lcd_background_size) * sizeof(std::uint32_t)) {
        pixels.resize(lcd_background_size);
        std::memcpy(pixels.data(), data, size);
    }
    g_bytes_unref(bytes);
    return pixels;
}

}  // namespace

Sc55View::Sc55View(std::function<double()> position) : position_(std::move(position)) {
    background_ = loadBackground();
    buffer_.assign(lcd_buffer_size, 0);

    stack_ = gtk_stack_new();
    owned_ = GObjectPtr<GtkWidget>::sink(stack_);
    gtk_widget_set_size_request(stack_, lcd_background_width / 3, lcd_background_height / 3);
    gtk_widget_set_hexpand(stack_, TRUE);
    gtk_widget_set_vexpand(stack_, TRUE);

    message_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(message_), TRUE);
    gtk_label_set_justify(GTK_LABEL(message_), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(message_), 40);
    gtk_widget_add_css_class(message_, "dim-label");
    gtk_widget_set_margin_start(message_, 12);
    gtk_widget_set_margin_end(message_, 12);
    gtk_stack_add_named(GTK_STACK(stack_), message_, "message");

    // Aspect preserved: the panel is a photograph of a real object, and a
    // stretched one looks like a mistake rather than a design.
    picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture_), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture_), TRUE);
    gtk_stack_add_named(GTK_STACK(stack_), picture_, "panel");

    // The clock follows the widget on and off the screen; the owner reports
    // whether the section is wanted and this reports whether it is seen.
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        stack_, "map", [this](GtkWidget*) { setActive(active_); }));
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        stack_, "unmap", [this](GtkWidget*) { timer_.stop(); }));

    showExplanation();
}

Sc55View::~Sc55View() { timer_.stop(); }

void Sc55View::setActive(bool active) {
    active_ = active;
    if (active && gtk_widget_get_mapped(stack_)) {
        timer_.start(kRefreshMs, [this] { tick(); });
        return;
    }
    timer_.stop();
    showExplanation();
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
    gtk_label_set_text(GTK_LABEL(message_), text.c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "message");
}

void Sc55View::tick() {
    if (background_.empty() || !position_) {
        return;
    }
    // A lookup, not a drain: what did the panel look like at the moment now
    // being heard. See PanelFeed::stateAt().
    const std::optional<PanelFrame> draw = PanelFeed::instance().stateAt(position_());
    if (!draw) {
        // Back to the explanation, and *back* is the direction that was
        // missing once: a panel that goes on drawing the last frame it was
        // handed is showing a machine that is no longer running.
        showExplanation();
        return;
    }
    if (draw->state.size() != sc55_lcd_state_size()) {
        return;
    }
    sc55_lcd_render_screen(background_.data(), buffer_.data(), draw->state.data(),
                           draw->state.size());

    // The visible rows, copied: the header says why a borrowed buffer is not
    // an option. The last row is only as wide as the panel, so the copy stops
    // at its end rather than at the stride's.
    const gsize stride = static_cast<gsize>(kStride) * sizeof(std::uint32_t);
    const gsize length = (static_cast<gsize>(lcd_background_height - 1) * stride) +
                         (static_cast<gsize>(lcd_background_width) * sizeof(std::uint32_t));
    GBytes*     bytes  = g_bytes_new(buffer_.data(), length);
    GdkTexture* frame  = gdk_memory_texture_new(lcd_background_width, lcd_background_height,
                                                GDK_MEMORY_R8G8B8X8, bytes, stride);
    g_bytes_unref(bytes);
    gtk_picture_set_paintable(GTK_PICTURE(picture_), GDK_PAINTABLE(frame));
    g_object_unref(frame);
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "panel");
}

}  // namespace xpcog::gtk
