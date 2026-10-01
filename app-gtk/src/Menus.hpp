// The menus, built from the shared command table.
//
// app-winui/src/CommandMenus.cpp turns menuLayout() into a WinUI MenuBar; this turns the same
// rows into GMenus, so the wording, the ordering and the translations are
// stated once. What differs is the shape, and deliberately: a libadwaita
// application has no menu bar. It has a primary menu under the header bar's
// button, a context menu on the thing being acted on, and shortcuts for the
// rest. So the File menu's opening commands sit under the header's add
// button, the playlist commands are the playlist's context menu, and the
// primary menu holds what is left -- the playback order, the panes, and the
// three items every GNOME application ends its primary menu with.

#pragma once

#include "Glib.hpp"

#include <gio/gio.h>

namespace xpcog::gtk {

/// The header bar's "add" menu: open files, folder, URL.
[[nodiscard]] GObjectPtr<GMenu> buildAddMenu();

/// The primary menu: save playlist, the order submenus, the panes submenu,
/// mini player, preferences, shortcuts, about.
[[nodiscard]] GObjectPtr<GMenu> buildPrimaryMenu();

/// The playlist's context menu, in Cog's order, plus the edit commands.
[[nodiscard]] GObjectPtr<GMenu> buildPlaylistMenu();

}  // namespace xpcog::gtk
