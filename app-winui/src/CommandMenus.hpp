#pragma once

#include "WinRT.hpp"

#include "Commands.hpp"  // uicore's table: app::CommandId, menuLayout()

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace xpcog::winui {

/// The window's menu and the playlist's context menu, built from uicore's
/// command table the way the wx and GTK frontends build theirs, so the three
/// cannot disagree about what a menu holds, what it is called or what its
/// shortcut is.
///
/// The menu is one flyout, opened from the title bar's menu button, with each
/// of the table's menus a submenu. Its shortcuts are KeyboardAccelerators on
/// the window's root rather than on the items -- a closed flyout's items are
/// not in the tree -- and each one checks its command is enabled when
/// invoked; the items show the key as text.
///
/// The enabled and checked states are refreshed as the menu opens and on the
/// events that change them, which is what GTK's refreshActionState() is
/// called for as well.
class CommandMenus {
public:
    struct Hooks {
        std::function<void(app::CommandId)> run;
        std::function<bool(app::CommandId)> enabled;
        /// nullopt for a command that is not a check or radio item.
        std::function<std::optional<bool>(app::CommandId)> checked;
        /// Commands the frontend has nowhere to put yet are left out of the
        /// menus entirely rather than shown dead.
        std::function<bool(app::CommandId)> offered;
    };

    explicit CommandMenus(Hooks hooks);

    /// The window's one menu, every menu of the table a submenu of it: what
    /// the title bar's menu button opens.
    [[nodiscard]] mux::Controls::MenuFlyout mainMenu() const { return mainMenu_; }

    /// The shortcuts, attached to `target` -- the window's root, so that a
    /// key pressed with focus anywhere in it reaches them.
    void attachAccelerators(const mux::UIElement& target);

    /// A fresh context menu, its states already current. Built each time, as
    /// the wx frame builds its popup, so nothing has to keep it up to date.
    [[nodiscard]] mux::Controls::MenuFlyout playlistMenu();

    /// Brings every menu bar item's enabled and checked state up to date.
    void refresh();

    /// Focus moved into or out of a text box. The shortcuts a text box has
    /// its own meaning for -- Delete, Ctrl+A, Ctrl+Z, Ctrl+Y, Ctrl+arrows --
    /// are switched off while it has focus, so that typing in the filter does
    /// not remove tracks or select the playlist.
    void setTextFocus(bool inText);

private:
    mux::Controls::MenuFlyoutItemBase makeItem(const app::MenuItem& row, bool withAccelerator,
                                              const std::wstring& radioGroup);
    void fill(const winrt::Windows::Foundation::Collections::IVector<mux::Controls::MenuFlyoutItemBase>& items,
              const std::vector<app::MenuItem>& rows, std::size_t first, std::size_t last,
              bool withAccelerators);
    void apply(const mux::Controls::MenuFlyoutItemBase& item, app::CommandId id) const;

    Hooks                  hooks_;
    mux::Controls::MenuFlyout mainMenu_{nullptr};
    /// Every shortcut, for attachAccelerators().
    std::vector<mux::Input::KeyboardAccelerator> accelerators_;
    /// The main menu's items by command, for refresh().
    std::multimap<app::CommandId, mux::Controls::MenuFlyoutItemBase> items_;
    /// The accelerators a text box also answers to.
    std::vector<mux::Input::KeyboardAccelerator> textKeys_;
    bool inText_ = false;
};

}  // namespace xpcog::winui
