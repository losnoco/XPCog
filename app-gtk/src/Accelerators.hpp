// The command table's accelerators, in GTK's spelling.
//
// The table writes "Ctrl+Shift+O", which is what wx parses and what a person
// reads. GTK wants "<Control><Shift>o": gtk_accelerator_parse's grammar, with
// the modifiers in angle brackets and the key as a keysym name. This is the
// one place that translation happens, and it is a pure function so a test can
// pin the spellings down without a display.

#pragma once

#include "Commands.hpp"

#include <gtk/gtk.h>

#include <string>
#include <string_view>

namespace xpcog::gtk {

/// "Ctrl+Shift+O" -> "<Control><Shift>o"; "Del" -> "Delete"; "" -> "".
[[nodiscard]] std::string gtkAccelerator(std::string_view tableSpelling);

/// The full action name a widget or a menu item refers to: "win.play-pause".
[[nodiscard]] std::string actionName(app::CommandId id);

/// The detailed name a menu item needs, with the radio target when there is
/// one: "win.repeat::all".
[[nodiscard]] std::string detailedActionName(app::CommandId id);

/// Whether this frontend offers `id` at all. The command table is shared with
/// the wx window, and a few of its rows are about things GTK 4 does not have:
/// "Dock Floating Panes" gathers torn-off panes back into the frame, and
/// libadwaita's panes cannot be torn off. Not offered means no action, no
/// menu row and no accelerator -- not a row that does nothing.
[[nodiscard]] bool offeredHere(app::CommandId id);

/// Installs every accelerator the table carries on `application`, so a
/// shortcut fires the same action the menu item does.
void installAccelerators(GtkApplication* application);

}  // namespace xpcog::gtk
