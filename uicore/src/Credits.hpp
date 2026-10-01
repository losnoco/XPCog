// What XPCog is built out of, for the About dialogs to credit.
//
// One table for both frontends, because a licence list has to be right rather
// than convenient: the wx and GTK dialogs each kept their own once, and the GTK
// one shipped 2.0.0 with none at all. The WinUI player's About reads it too. Kept by hand rather than generated -- a
// library dropped from the build should be removed from here deliberately, not
// vanish silently, and a library added should be credited by someone who read
// its licence.
//
// Four groups, because they answer different questions. The player's own
// components are in every build; the toolkit rows depend on which frontend is
// running; the decoders a given build contains depend on how it was configured
// (the Formats list says what *this* build plays); and the data rows are not
// code at all but are compiled in or shipped beside the binary, and are
// credited to whoever made them whether or not a licence was ever stated.

#pragma once

#include <span>

namespace xpcog::app {

struct Component {
    /// A proper noun, never translated.
    const char* name;
    /// The licence as a reader would recognise it -- "public domain", "zlib
    /// licence" -- or for data the rights holder. An SPDX identifier where a
    /// toolkit has a licence of its own for it (MIT, Apache-2.0, BSD-3-Clause,
    /// the GPL and LGPL "-or-later" and "-only" forms, MPL-2.0), so the GTK
    /// Legal page can link the real text; SPDX's names for the rest, like
    /// "blessing" for SQLite's, mean nothing on screen. Never translated: it
    /// is checked against the component, not read as prose.
    const char* licence;
    /// What it does here. Marked for translation; look it up with tr().
    const char* purpose;
};

/// In every build that has a player.
[[nodiscard]] std::span<const Component> playerComponents();

/// The Windows frontend's toolkit and what it draws with.
[[nodiscard]] std::span<const Component> winuiComponents();

/// The Linux frontend's toolkit, and the desktop libraries platform/ talks to.
[[nodiscard]] std::span<const Component> gtkComponents();

/// The decoder and tag libraries, whichever of them this build contains.
[[nodiscard]] std::span<const Component> codecComponents();

/// Data compiled into the binary or shipped beside it.
[[nodiscard]] std::span<const Component> dataComponents();

}  // namespace xpcog::app
