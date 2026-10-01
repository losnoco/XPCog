// The wx half of the command tables.
//
// Commands.hpp is the tables themselves -- the ids, the layout rows, the labels
// and the accelerators as written -- and it names no toolkit, because both
// frontends read it. This is what turns those rows into wxMenuBar, wxMenu and
// wxItemKind, and it is the only part of the command surface that a second
// frontend replaces rather than reuses.

#pragma once

#include "Commands.hpp"

#include <wx/defs.h>
#include <wx/string.h>

#include <vector>

class wxMenu;
class wxMenuBar;

namespace xpcog::app {

/// The command's label plus its accelerator in parentheses where it has one:
/// "Next (Ctrl+Right)". For a tooltip, which is the only place a surface showing
/// icons alone can say either.
///
/// Here rather than beside commandLabel() because the accelerator is *rendered*
/// by wx rather than pasted in: the table spells every shortcut `Ctrl`, which wx
/// turns into Cmd when it builds the real one, so macOS reads its own modifier
/// symbols instead of the table's literal.
[[nodiscard]] wxString commandTooltip(CommandId id);

/// The wx spelling of an ItemKind, for a surface that builds its own items
/// rather than going through buildMenu().
[[nodiscard]] wxItemKind toWxItemKind(ItemKind kind);

/// Builds the whole menu bar from the table in Commands.hpp.
[[nodiscard]] wxMenuBar* buildMenuBar();

/// Builds one standalone menu from a table -- a popup, which has no bar to hang
/// off. The caller owns it; pop it up and let it go out of scope.
[[nodiscard]] wxMenu* buildMenu(const std::vector<MenuItem>& items);

}  // namespace xpcog::app
