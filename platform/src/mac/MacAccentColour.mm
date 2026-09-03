#include <xpcog/platform/AccentColour.hpp>

#import <AppKit/AppKit.h>

namespace xpcog::platform {

std::optional<AccentRgb> accentColour() {
    @autoreleasepool {
        // controlAccentColor is a dynamic colour: it resolves differently in
        // light and dark, and it resolves to the *graphite* grey rather than a
        // hue when the user has picked that. Converting it to a concrete RGB is
        // therefore something to do per call, against the appearance in force,
        // not once at startup.
        NSColor* accent = [NSColor controlAccentColor];

        // -getRed:green:blue:alpha: raises for a colour that is not already in an
        // RGB space, and a system colour is in a catalog space. The conversion
        // can fail -- it returns nil rather than raising -- so it is checked.
        NSColor* rgb = [accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (rgb == nil) {
            return std::nullopt;
        }

        CGFloat r = 0.0;
        CGFloat g = 0.0;
        CGFloat b = 0.0;
        CGFloat a = 0.0;
        [rgb getRed:&r green:&g blue:&b alpha:&a];

        const auto to8 = [](CGFloat component) {
            const CGFloat clamped = component < 0.0 ? 0.0 : (component > 1.0 ? 1.0 : component);
            return static_cast<std::uint8_t>((clamped * 255.0) + 0.5);
        };

        return AccentRgb{to8(r), to8(g), to8(b)};
    }
}

}  // namespace xpcog::platform
