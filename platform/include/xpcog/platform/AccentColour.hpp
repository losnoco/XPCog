// The colour the user chose for their desktop.
//
// Not the same thing as a selection highlight, which is what a toolkit will hand
// you if you ask for the nearest thing it knows. macOS has had
// NSColor.controlAccentColor since Mojave and it is what the system's own
// sliders, switches and progress bars are tinted with; Windows keeps the same
// idea in UISettings' Accent colour, which is what the taskbar and title bars
// pick up. wxWidgets 3.3 exposes neither -- wxSYS_COLOUR_HIGHLIGHT is the
// selection colour, which on macOS is a *derived* pastel of the accent and on
// Windows is a different colour entirely.
//
// So a control that wants to look like it belongs on the desktop has to ask the
// OS, and this is where that question goes.
//
// Nothing on Linux, deliberately. There is no desktop-wide accent to read: GTK
// themes carry a selection colour and nothing else, which is exactly what
// wxSYS_COLOUR_HIGHLIGHT already returns there. Returning nothing lets the
// caller fall back to it without a per-platform branch of its own -- and the
// fallback is the right answer rather than a degraded one.
//
// No handle argument: this is a property of the session, not of a window.

#pragma once

#include <cstdint>
#include <optional>

namespace xpcog::platform {

/// Straight 8-bit sRGB, which is what every caller here wants and what both
/// platform APIs are converted into. No alpha: an accent colour is opaque, and a
/// caller that wants it faded says so itself.
struct AccentRgb {
    std::uint8_t red   = 0;
    std::uint8_t green = 0;
    std::uint8_t blue  = 0;
};

/// The desktop's accent colour, or nothing where the platform has none.
///
/// Cheap enough to call from a paint handler -- both implementations read a
/// cached system value rather than doing any work -- which is deliberate: the
/// colour can change while the application is running, and a value read once at
/// construction would be stale until the next launch.
[[nodiscard]] std::optional<AccentRgb> accentColour();

}  // namespace xpcog::platform
