#include "Menus.hpp"

#include "Accelerators.hpp"
#include "Commands.hpp"
#include "Translations.hpp"

#include <string>
#include <string_view>

namespace xpcog::gtk {

using app::CommandId;
using app::MenuItem;

namespace {

/// The table's label in GTK's mnemonic spelling: `&` marks the underline in
/// wx and `_` does in GTK, and `&&` is a literal ampersand there where `__`
/// is a literal underscore here.
std::string gtkLabel(const char* msgid) {
    const std::string translated = app::tr(msgid);
    std::string       out;
    out.reserve(translated.size());
    for (std::size_t i = 0; i < translated.size(); ++i) {
        const char c = translated[i];
        if (c == '&') {
            if (i + 1 < translated.size() && translated[i + 1] == '&') {
                out.push_back('&');
                ++i;
            } else {
                out.push_back('_');
            }
        } else if (c == '_') {
            out += "__";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

const MenuItem* rowFor(CommandId id) {
    for (const MenuItem& item : app::menuLayout()) {
        if (item.id == id) {
            return &item;
        }
    }
    for (const MenuItem& item : app::playlistMenuLayout()) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

/// One item, labelled from the table and bound to the command's action.
void appendCommand(GMenu* section, CommandId id) {
    const MenuItem* row = rowFor(id);
    if (row == nullptr || !offeredHere(id)) {
        return;
    }
    const std::string label    = gtkLabel(row->label);
    const std::string detailed = detailedActionName(id);
    g_menu_append(section, label.c_str(), detailed.c_str());
}

/// The rows of one of the table's top-level menus, in order.
std::vector<const MenuItem*> rowsOfMenu(const char* title) {
    std::vector<const MenuItem*> rows;
    bool                         inside = false;
    for (const MenuItem& item : app::menuLayout()) {
        if (item.menu != nullptr) {
            inside = std::string_view(item.menu) == title;
        }
        if (inside) {
            rows.push_back(&item);
        }
    }
    return rows;
}

/// Appends `rows` to `menu`, starting a new section at every separator.
void appendSections(GMenu* menu, const std::vector<const MenuItem*>& rows) {
    GObjectPtr<GMenu> section = GObjectPtr<GMenu>::adopt(g_menu_new());
    for (const MenuItem* row : rows) {
        if (row->separatorBefore && g_menu_model_get_n_items(G_MENU_MODEL(section.get())) > 0) {
            g_menu_append_section(menu, nullptr, G_MENU_MODEL(section.get()));
            section = GObjectPtr<GMenu>::adopt(g_menu_new());
        }
        appendCommand(section.get(), row->id);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(section.get())) > 0) {
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(section.get()));
    }
}

}  // namespace

GObjectPtr<GMenu> buildAddMenu() {
    auto menu    = GObjectPtr<GMenu>::adopt(g_menu_new());
    auto section = GObjectPtr<GMenu>::adopt(g_menu_new());
    appendCommand(section.get(), CommandId::FileOpen);
    appendCommand(section.get(), CommandId::FileOpenFolder);
    appendCommand(section.get(), CommandId::FileOpenUrl);
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(section.get()));
    return menu;
}

GObjectPtr<GMenu> buildPrimaryMenu() {
    auto menu = GObjectPtr<GMenu>::adopt(g_menu_new());

    auto playlist = GObjectPtr<GMenu>::adopt(g_menu_new());
    appendCommand(playlist.get(), CommandId::FileSavePlaylist);
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(playlist.get()));

    // The Order menu, as two submenus: the table's separator is where repeat
    // ends and shuffle begins.
    auto order   = GObjectPtr<GMenu>::adopt(g_menu_new());
    auto repeat  = GObjectPtr<GMenu>::adopt(g_menu_new());
    auto shuffle = GObjectPtr<GMenu>::adopt(g_menu_new());
    for (const MenuItem* row : rowsOfMenu("&Order")) {
        const app::CommandAction action = app::commandAction(row->id);
        if (std::string_view(action.name) == "repeat") {
            appendCommand(repeat.get(), row->id);
        } else {
            appendCommand(shuffle.get(), row->id);
        }
    }
    g_menu_append_submenu(order.get(), gtkLabel(XPCOG_TRANSLATE("&Repeat")).c_str(),
                          G_MENU_MODEL(repeat.get()));
    g_menu_append_submenu(order.get(), gtkLabel(XPCOG_TRANSLATE("&Shuffle")).c_str(),
                          G_MENU_MODEL(shuffle.get()));
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(order.get()));

    // The View menu, whole, as the panels submenu -- plus the mini player
    // beside it, which is a mode of the window rather than a pane.
    auto view   = GObjectPtr<GMenu>::adopt(g_menu_new());
    auto panels = GObjectPtr<GMenu>::adopt(g_menu_new());
    std::vector<const MenuItem*> paneRows;
    for (const MenuItem* row : rowsOfMenu("&View")) {
        if (row->id != CommandId::ViewMiniPlayer) {
            paneRows.push_back(row);
        }
    }
    appendSections(panels.get(), paneRows);
    g_menu_append_submenu(view.get(), gtkLabel(XPCOG_TRANSLATE("&Panels")).c_str(),
                          G_MENU_MODEL(panels.get()));
    appendCommand(view.get(), CommandId::ViewMiniPlayer);
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(view.get()));

    // What every GNOME primary menu ends with.
    auto tail = GObjectPtr<GMenu>::adopt(g_menu_new());
    appendCommand(tail.get(), CommandId::FilePreferences);
    g_menu_append(tail.get(), gtkLabel(XPCOG_TRANSLATE("&Keyboard Shortcuts")).c_str(),
                  "win.shortcuts");
    appendCommand(tail.get(), CommandId::HelpAbout);
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(tail.get()));

    return menu;
}

GObjectPtr<GMenu> buildPlaylistMenu() {
    auto menu = GObjectPtr<GMenu>::adopt(g_menu_new());

    // Cog's context menu rows, in Cog's order, with the table's separators.
    std::vector<const MenuItem*> rows;
    for (const MenuItem& item : app::playlistMenuLayout()) {
        rows.push_back(&item);
    }
    appendSections(menu.get(), rows);

    // And the Edit menu's commands, which have no menu bar to live on here.
    auto edit = GObjectPtr<GMenu>::adopt(g_menu_new());
    for (const MenuItem* row : rowsOfMenu("&Edit")) {
        if (row->id == CommandId::EditRemove) {
            continue;  // already on the context menu, under Cog's name for it
        }
        appendCommand(edit.get(), row->id);
    }
    g_menu_append_section(menu.get(), nullptr, G_MENU_MODEL(edit.get()));

    return menu;
}

}  // namespace xpcog::gtk
