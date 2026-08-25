// Which language the interface speaks.
//
// The translations are compiled into the binary from `app/locale/*.po` -- see
// cmake/XPCogCatalogs.cmake for why they are not files beside the executable --
// and this is the layer between that generated table and wxWidgets' own gettext
// machinery. Everything above it uses `_()`, `wxPLURAL()` and `wxTRANSLATE()`
// and knows nothing about where a catalogue came from.
//
// **This is app-layer only, and deliberately.** `core`, `codecs` and `platform`
// link no toolkit, so they have no `_()` to call and no catalogue to call it
// against -- see the layering rule in CLAUDE.md. The few strings they do produce
// that a listener ever reads are translated where they are shown: the playlist's
// column headings by PlaylistDataModel, which is what
// `PlaylistView::heading()`'s "a front end that wants them localised should map
// them" was written for.

#pragma once

#include "catalogs.hpp"

#include <string>
#include <vector>

namespace xpcog::app {

/// One entry in the Preferences picker.
struct LanguageOption {
    std::string code;  ///< the stored setting; empty means "follow the system"
    std::string name;  ///< what that language calls itself, in that language
};

/// English, every compiled-in catalogue, and the system option in front.
///
/// English is always here and is never a catalogue: it is the language the
/// msgids are written in, so choosing it means loading nothing at all.
[[nodiscard]] std::vector<LanguageOption> availableLanguages();

/// The `.mo` image a catalogue assembles to.
///
/// Exposed for the test rather than for callers. wxMsgCatalog can be built from
/// a file or from a block of bytes in gettext's binary format and from nothing
/// else, so this is the one shape a compiled-in catalogue can reach wx in --
/// which makes the offset arithmetic below worth pinning down independently of
/// whether a window happens to come out in Spanish.
[[nodiscard]] std::string assembleCatalog(const Catalog& catalog);

/// Installs the catalogues and chooses one. Called once, before any window.
///
/// `language` is the stored setting: a catalogue's code, `"en"`, or empty for
/// whatever the system asks for. A code this build has no catalogue for falls
/// back to the system's choice rather than to silence.
void installTranslations(const std::string& language);

}  // namespace xpcog::app
