// Translation without a toolkit.
//
// The compiled-in catalogues, read directly: what both players look their
// strings up through. It began as the second of two lookups, beside the wx
// player's, which handed the same table to wxTranslations; since 3.0.0 it is
// the only one.
//
// **There is no trUtf8() here, and no need for one.** The wx player had to spell
// a message whose English was not pure ASCII a second way, because
// `wxString(const char*)` decoded through the current 8-bit locale on Windows.
// Nothing here goes near a wxString: a msgid is UTF-8 bytes in, and a
// std::string of UTF-8 bytes out. One function, every message, including the
// ones carrying real typography.

#pragma once

#include "catalogs.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::app {

/// The `.mo` image a catalogue assembles to: gettext's binary format, little
/// endian, sorted, no hash table.
///
/// Here rather than beside one toolkit because both frontends need it, for
/// different consumers. wxMsgCatalog can be built from a block of bytes in this
/// format and from nothing else; GtkBuilder translates a `.ui` file's strings
/// through the C library's gettext, which reads this format from disk. The
/// compiled-in table is the source of truth for both, and this is the one
/// shape it reaches either in -- which makes the offset arithmetic worth
/// pinning down in a test independently of whether a window comes out in
/// Spanish.
[[nodiscard]] std::string assembleCatalog(const Catalog& catalog);

/// Selects the catalogue for `language`, by the code Localization uses ("es").
///
/// Named apart from Localization.hpp's installTranslations because the two share
/// a namespace and take the same argument, and a call resolving to the wrong one
/// would compile: this installs the lookup, that one installs both and is what a
/// frontend calls.
///
/// A language this build has no catalogue for -- and the empty string, which is
/// "the system's choice" resolving to English -- leaves the msgids as written.
/// That is a working interface, not a degraded one, which is why it is not an
/// error.
///
/// Called once, before anything is drawn, from Localization::installTranslations
/// beside wxTranslations::Set -- so the two lookups cannot disagree about which
/// language is loaded.
void installNeutralTranslations(const std::string& language);

/// The language currently installed, or empty for "the msgids as written".
[[nodiscard]] const std::string& currentLanguage();

/// One entry in the Preferences picker.
struct LanguageOption {
    std::string code;  ///< the stored setting; empty means "follow the system"
    std::string name;  ///< what that language calls itself, in that language
};

/// English, every compiled-in catalogue, and the system option in front.
///
/// English is always here and is never a catalogue: it is the language the
/// msgids are written in, so choosing it means loading nothing at all.
///
/// Here rather than in Localization.hpp because it is a question about the
/// build, not about a toolkit -- a preferences pane painted by anything needs
/// the same list.
[[nodiscard]] std::vector<LanguageOption> availableLanguages();

/// The catalogue's answer for `msgid`, or `msgid` itself.
///
/// This is what `_()` and `trUtf8()` both are; see the header comment for why
/// one function covers both.
[[nodiscard]] std::string tr(const char* msgid);

/// The plural form for `n`, chosen by the catalogue's own Plural-Forms rule.
///
/// This is `wxPLURAL`. The rule matters: Spanish agrees with English, but the
/// languages that do not are exactly the ones a hand-written `n == 1` would get
/// wrong, and silently.
[[nodiscard]] std::string trn(const char* singular, const char* plural,
                              std::size_t n);

/// Marks a literal for tools/extract-messages.py without translating it.
///
/// This is `wxTRANSLATE`. The command tables are read by three surfaces and
/// translated when a menu is built rather than when the table is, because the
/// language can change under all three.
#define XPCOG_TRANSLATE(str) str

namespace detail {

/// One argument, already rendered.
///
/// Arguments are flattened to text before the format is walked, which is what
/// lets `substitute` be a plain function over a vector rather than a variadic
/// that has to re-derive each type at every specifier.
[[nodiscard]] std::string toText(std::string value);
[[nodiscard]] std::string toText(std::string_view value);
[[nodiscard]] std::string toText(const char* value);
[[nodiscard]] std::string toText(int value);
[[nodiscard]] std::string toText(unsigned value);
[[nodiscard]] std::string toText(long value);
[[nodiscard]] std::string toText(unsigned long value);
[[nodiscard]] std::string toText(long long value);
[[nodiscard]] std::string toText(unsigned long long value);

/// Replaces each `%s`, `%zu`, `%d` and `%u` in `format` with the next argument,
/// and `%%` with a literal `%`.
///
/// Not snprintf, and not because of the warning -Wformat=2 would raise about a
/// runtime format string. A format string that came out of a catalogue is
/// attacker-adjacent input in the one place C makes that catastrophic: a `%n` a
/// translator typed by accident, or a catalogue swapped by something else on the
/// machine, is an arbitrary write. This knows four specifiers and cannot be
/// asked to do anything else.
///
/// A specifier with no argument left is emitted as it stands, and a surplus
/// argument is dropped. Both mean the translation disagrees with its msgid, and
/// neither is worth losing the message over: the reader still gets a sentence.
[[nodiscard]] std::string substitute(std::string_view       format,
                                     const std::vector<std::string>& args);

}  // namespace detail

/// tr() followed by the substitution above. This is `wxString::Format(_(...))`.
///
/// Separate from tr() so tools/extract-messages.py still sees a plain literal at
/// the call site, and so the msgid keeps its `%s` and `%zu` spellings -- the
/// existing es.po is written against them, and changing them to `{}` would
/// orphan every entry that has one.
template <typename... Args>
[[nodiscard]] std::string trf(const char* msgid, Args&&... args) {
    return detail::substitute(tr(msgid),
                              {detail::toText(std::forward<Args>(args))...});
}

/// The same substitution over a string that is already translated.
///
/// For the case AppCommands has: a label built by wrapping one translated
/// message in another, where the inner one must be looked up before the outer
/// one formats it.
template <typename... Args>
[[nodiscard]] std::string fmt(std::string_view format, Args&&... args) {
    return detail::substitute(format,
                              {detail::toText(std::forward<Args>(args))...});
}

}  // namespace xpcog::app
