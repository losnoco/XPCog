// The UTF-8 boundary, in one place.
//
// This is the highest-probability silent bug in the whole port, so it gets a
// header of its own and a rule that can be grepped for.
//
// `QString::fromStdString` is UTF-8 in Qt 6. **`wxString(const char*)` is not.**
// On Windows it decodes using the current 8-bit locale, so a mechanical
// translation of the 294 QString uses this application had -- most of them
// wrapping tag text straight out of a PlaylistEntry -- produces mojibake on every
// non-ASCII tag, on one platform, and passes CI, whose fixtures are ASCII.
//
// The rule: **a wxString is constructed from a string literal, or from toWx().
// Never from a std::string or a const char* holding data.**
//
// Everything below this layer speaks UTF-8 in a std::string, which is what core
// uses and what the platform headers say. These two functions are the only places
// that should change.

#pragma once

#include <wx/string.h>

#include <string>
#include <string_view>

namespace xpcog::app {

[[nodiscard]] inline wxString toWx(std::string_view text) {
    return wxString::FromUTF8(text.data(), text.size());
}

[[nodiscard]] inline std::string toUtf8(const wxString& text) {
    return text.utf8_string();
}

}  // namespace xpcog::app
