#pragma once

// The colour arithmetic the painted panes share: app-gtk/src/Painting.hpp's,
// over WinRT's Color rather than GdkRGBA, so the two players' visualisers
// shade a bar by the same rule and come out the same colours.

#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

namespace xpcog::winui {

using Colour = winrt::Windows::UI::Color;

/// The settings' colours: "#rgb", "#rrggbb" or "#rrggbbaa", which is what the
/// preference pickers write and what Cog's defaults are spelled as.
[[nodiscard]] inline std::optional<Colour> parseColour(std::string_view text) {
    if (text.empty() || text.front() != '#') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int digits[8] = {};
    for (std::size_t i = 0; i < text.size() && i < 8; ++i) {
        if ((digits[i] = nibble(text[i])) < 0) {
            return std::nullopt;
        }
    }
    const auto byte = [&](int high, int low) { return static_cast<uint8_t>((high << 4) | low); };
    switch (text.size()) {
        case 3:
            return Colour{255, byte(digits[0], digits[0]), byte(digits[1], digits[1]),
                          byte(digits[2], digits[2])};
        case 6:
            return Colour{255, byte(digits[0], digits[1]), byte(digits[2], digits[3]),
                          byte(digits[4], digits[5])};
        case 8:
            return Colour{byte(digits[6], digits[7]), byte(digits[0], digits[1]),
                          byte(digits[2], digits[3]), byte(digits[4], digits[5])};
        default:
            return std::nullopt;
    }
}

[[nodiscard]] constexpr Colour rgba(float r, float g, float b, float a = 1.0F) {
    const auto to = [](float v) {
        return static_cast<uint8_t>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F);
    };
    return Colour{to(a), to(r), to(g), to(b)};
}

[[nodiscard]] inline Colour withAlpha(Colour colour, float alpha) {
    colour.A = static_cast<uint8_t>(std::clamp(alpha, 0.0F, 1.0F) * 255.0F + 0.5F);
    return colour;
}

/// Darker for a factor above 100, lighter below: wxColour::ChangeLightness's
/// rule as GTK's shade() has it.
[[nodiscard]] inline Colour shade(Colour colour, int factor) {
    const auto scale = [factor](uint8_t channel) {
        return static_cast<uint8_t>(
            std::clamp(static_cast<int>(channel) * 100 / factor, 0, 255));
    };
    colour.R = scale(colour.R);
    colour.G = scale(colour.G);
    colour.B = scale(colour.B);
    return colour;
}

}  // namespace xpcog::winui
