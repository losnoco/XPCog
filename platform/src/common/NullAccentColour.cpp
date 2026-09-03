// Linux, and anywhere else without a desktop-wide accent colour.
//
// Not a stub standing in for work not done: there is no such colour to read on a
// GTK desktop. A theme has a selection colour, which is what
// wxSYS_COLOUR_HIGHLIGHT already returns, so the caller's fallback is the
// correct answer rather than an approximation of one. See AccentColour.hpp.

#include <xpcog/platform/AccentColour.hpp>

namespace xpcog::platform {

std::optional<AccentRgb> accentColour() { return std::nullopt; }

}  // namespace xpcog::platform
