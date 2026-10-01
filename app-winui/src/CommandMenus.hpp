#pragma once

#include "WinRT.hpp"

#include "Commands.hpp"  // uicore's table: app::CommandId, menuLayout()

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace xpcog::winui {

/// The menu bar and the playlist's context menu, built from uicore's command
/// table the way the wx and GTK frontends build theirs, so the three cannot
/// disagree about what a menu holds, what it is called or what its shortcut is.
///
/// Shortcuts are KeyboardAccelerators on the menu bar's items, which WinUI
/// treats as global: they fire with the menu closed and focus anywhere in the
/// window. The context menu shows the same shortcut text without registering
/// a second accelerator for it.
///
/// A disabled item's accelerator does not fire, so the enabled states are kept
/// current all the time rather than when a menu opens -- refresh() after
/// anything that changes them, which is what GTK's refreshActionState() is
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

    [[nodiscard]] mux::Controls::MenuBar menuBar() const { return menuBar_; }

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
    mux::Controls::MenuBar menuBar_{nullptr};
    /// The menu bar's items by command, for refresh().
    std::multimap<app::CommandId, mux::Controls::MenuFlyoutItemBase> items_;
    /// The accelerators a text box also answers to.
    std::vector<mux::Input::KeyboardAccelerator> textKeys_;
    bool inText_ = false;
};

}  // namespace xpcog::winui
