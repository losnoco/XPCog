#pragma once

// Window furniture more than one window here builds: the transport's glyph
// buttons, and a title bar whose content takes the bar's whole free width.

#include "WinRT.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace xpcog::winui {

// Segoe Fluent Icons code points: the system's own glyphs for these, the same
// ones Media Player draws.
inline constexpr const wchar_t* kGlyphPlay     = L"\xE768";
inline constexpr const wchar_t* kGlyphPause    = L"\xE769";
inline constexpr const wchar_t* kGlyphStop     = L"\xE71A";
inline constexpr const wchar_t* kGlyphPrevious = L"\xE892";
inline constexpr const wchar_t* kGlyphNext     = L"\xE893";
inline constexpr const wchar_t* kGlyphVolume   = L"\xE767";

/// A subtle button holding one glyph: no fill and no border at rest, the
/// theme's hover and pressed fills when touched. `tooltip` is its accessible
/// name as well.
[[nodiscard]] mux::Controls::Button glyphButton(const wchar_t* glyph, const std::string& tooltip,
                                                std::function<void()> action);

/// Sets a glyph button's glyph and name: Play/Pause, which changes with the
/// transport.
void setGlyph(const mux::Controls::Button& button, const wchar_t* glyph, const std::string& tooltip);

/// Gives `content`, the title bar's Content, the width of the template's
/// content column for as long as the bar lives.
///
/// The TitleBar template puts its content in a presenter aligned by a theme
/// resource -- centred, for a search box -- and its compact state pins it Left
/// whatever the resource says (microsoft-ui-xaml #11181). Either way content
/// gets only the width it asks for: a menu bar is centred, and a seek bar's
/// star column comes to nothing. Sized outright, it lays out across what is
/// left once the icon, title, headers and caption buttons have theirs.
void fitTitleBarContent(const mux::Controls::TitleBar& bar, const mux::FrameworkElement& content);

/// A file beside the executable: the icon, which AppWindow and the tray take
/// as a path.
[[nodiscard]] std::filesystem::path besideExecutable(const wchar_t* name);

}  // namespace xpcog::winui
