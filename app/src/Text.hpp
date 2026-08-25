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
#include <wx/translation.h>

#include <string>
#include <string_view>

namespace xpcog::app {

[[nodiscard]] inline wxString toWx(std::string_view text) {
    return wxString::FromUTF8(text.data(), text.size());
}

[[nodiscard]] inline std::string toUtf8(const wxString& text) {
    return text.utf8_string();
}

// --- the same rule, on the way into the catalogue -------------------------
//
// `_()` has the defect this header exists to ban, and it is not obvious from
// looking at it. It expands to `wxGetTranslation(const wxString&)` and lets the
// literal convert *implicitly* -- which on Windows is the current 8-bit locale,
// exactly as above. So a msgid carrying an em dash, a `±`, a `©` or a `×` is
// looked up under a mangled key, misses, and is then displayed mangled. It
// compiles, it passes a test suite whose fixtures are ASCII, and it is visible
// only to somebody reading the About box or the Rubber Band pane.
//
// So the rule has a second half. **A message whose English is not pure ASCII
// goes through `trUtf8()`; everything else uses `_()` and `wxPLURAL()`.**
// `tools/extract-messages.py` reads all four, and refuses to write a template
// when a non-ASCII message has been marked with the wrong one -- which is what
// keeps this from being a rule people have to remember.
//
// A msgid that is pure ASCII is safe through `_()` on every platform, and most
// of them are: this is for the two dozen that carry real typography.

[[nodiscard]] inline wxString trUtf8(const char* text) {
    return wxGetTranslation(wxString::FromUTF8(text));
}

[[nodiscard]] inline wxString trUtf8(const char* singular, const char* plural,
                                     unsigned count) {
    return wxGetTranslation(wxString::FromUTF8(singular), wxString::FromUTF8(plural),
                            count);
}

}  // namespace xpcog::app
