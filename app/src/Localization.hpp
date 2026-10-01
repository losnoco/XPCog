// Which language the interface speaks.
//
// The translations are compiled into the binary from `app/locale/*.po` -- see
// cmake/XPCogCatalogs.cmake for why they are not files beside the executable --
// and this is the layer between that generated table and wxWidgets' own gettext
// machinery. Everything above it uses `_()`, `wxPLURAL()` and `wxTRANSLATE()`
// and knows nothing about where a catalogue came from.
//
// **What is left here is app-layer only, and deliberately.** The picker's list
// of languages and the lookup itself are in Translations.hpp, which names no
// toolkit; what stays is the part that only exists because wx is on the other
// side of it -- the loader that hands wxMsgCatalog the `.mo` image
// Translations.hpp assembles, and the one call that installs both lookups.
// `core`, `codecs` and `platform`
// link no toolkit, so they have no `_()` to call and no catalogue to call it
// against -- see the layering rule in CLAUDE.md. The few strings they do produce
// that a listener ever reads are translated where they are shown: the playlist's
// column headings by PlaylistColumns, which is what
// `PlaylistView::heading()`'s "a front end that wants them localised should map
// them" was written for.

#pragma once

#include "catalogs.hpp"

#include <string>
#include <vector>

namespace xpcog::app {

/// Installs the catalogues and chooses one. Called once, before any window.
///
/// `language` is the stored setting: a catalogue's code, `"en"`, or empty for
/// whatever the system asks for. A code this build has no catalogue for falls
/// back to the system's choice rather than to silence.
void installTranslations(const std::string& language);

}  // namespace xpcog::app
