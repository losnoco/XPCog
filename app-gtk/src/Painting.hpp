// What the painted widgets share: colours as GdkRGBA, read from settings or
// from the theme, and the few arithmetic tricks the wx panels do on them.
//
// The wx panels do their colour arithmetic on wxColour; these are the same
// operations on GdkRGBA, whose channels are floats in 0..1 rather than bytes,
// so the factors are the same and the clamps are not.

#pragma once

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <optional>
#include <string>

namespace xpcog::gtk {

/// The setting's text as a colour, or nothing for text that is not one: a
/// hand-edited settings file has a fair chance of meaning what it says, and
/// what it does not say should keep what was there rather than turn black.
[[nodiscard]] inline std::optional<GdkRGBA> parseColour(const std::string& text) {
    GdkRGBA rgba;
    if (!text.empty() && gdk_rgba_parse(&rgba, text.c_str())) {
        return rgba;
    }
    return std::nullopt;
}

[[nodiscard]] inline GdkRGBA withAlpha(GdkRGBA colour, float alpha) {
    colour.alpha = alpha;
    return colour;
}

/// Qt's QColor::darker()/lighter(), as the wx panels spell it: a percentage,
/// 160 for "160% darker", i.e. each channel scaled by 100/160.
[[nodiscard]] inline GdkRGBA shade(GdkRGBA colour, int factor) {
    const auto scale = [factor](float channel) {
        return std::clamp(channel * 100.0F / static_cast<float>(factor), 0.0F, 1.0F);
    };
    colour.red   = scale(colour.red);
    colour.green = scale(colour.green);
    colour.blue  = scale(colour.blue);
    return colour;
}

inline void setSource(cairo_t* cr, const GdkRGBA& colour) { gdk_cairo_set_source_rgba(cr, &colour); }

/// The widget's text colour: the one colour guaranteed to read against its
/// background in either appearance and in whatever theme is running, which is
/// why outlines and baselines are derived from it rather than named.
[[nodiscard]] inline GdkRGBA foreground(GtkWidget* widget) {
    GdkRGBA colour;
    gtk_widget_get_color(widget, &colour);
    return colour;
}

}  // namespace xpcog::gtk
