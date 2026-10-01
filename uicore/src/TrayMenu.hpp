// What the tray icon says: its tooltip and its menu, decided without a
// toolkit so both frontends publish the same thing to the same panel.
//
// The menu is Cog's dock menu: the track as two disabled rows at the top, the
// transport, and -- where there is a tray rather than a Dock -- a row to bring
// the window back and one to quit. The ids are CommandIds, so the frontend
// runs them through the same switch its buttons and menus reach; the one
// that is not a command, Show Window, has an id of its own here.

#pragma once

#include "Commands.hpp"

#include "xpcog/platform/TrayIcon.hpp"

#include <string>
#include <vector>

namespace xpcog::app {

/// The tray menu's "Show XPCog" row: not a command, so an id past them all.
inline constexpr int kTrayShowWindowId = FirstWidgetId + 80;

/// What the tray shows for the transport.
struct TrayState {
    std::string title;
    std::string artist;
    bool        playing = false;
    bool        paused  = false;
};

/// Long titles get cut rather than stretching a tooltip across the screen,
/// on a character boundary, with an ellipsis.
[[nodiscard]] std::string elideForTray(const std::string& text);

/// The tooltip's body under the "XPCog" title: the track, and "(paused)".
[[nodiscard]] std::string trayTooltipBody(const TrayState& state);

/// The menu. `withWindowItems` adds Show and Quit, which a tray needs and a
/// Dock menu must not have twice.
[[nodiscard]] std::vector<platform::TrayMenuItem> trayMenuModel(const TrayState& state,
                                                                bool withWindowItems);

}  // namespace xpcog::app
