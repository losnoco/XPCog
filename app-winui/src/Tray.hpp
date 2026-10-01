#pragma once

// The notification-area icon: the transport out of the way, and the place a
// window closed to the tray comes back from.
//
// Shell_NotifyIcon by hand, because WinUI has no tray of its own and the wx
// player's was wxTaskBarIcon. The menu is uicore's trayMenuModel(), the one
// the wx and GTK trays render, so the three cannot describe different menus;
// its ids are CommandIds, run through the window's own command switch, with
// kTrayShowWindowId the one that is not a command.
//
// The icon's messages go to a hidden window of its own rather than to the
// player's: the player's window may be hidden, closed to this very icon, and
// WinUI owns its message loop. A hidden top-level window rather than a
// message-only one, because TrackPopupMenu wants a window that can be brought
// to the foreground, and a message-only window cannot -- the menu would not
// close when clicked away from.

#include "TrayMenu.hpp"

#include <windows.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace xpcog::winui {

class Tray {
public:
    Tray();
    ~Tray();

    Tray(const Tray&)            = delete;
    Tray& operator=(const Tray&) = delete;

    /// Whether the icon is in the notification area. False where the shell
    /// refused it, and then nothing may hide the window on its account.
    [[nodiscard]] bool shown() const noexcept { return shown_; }

    /// What the tooltip and the menu say about the transport.
    void setState(const app::TrayState& state);

    /// A notice from the icon: a toast, on Windows 10 and later, with the icon's
    /// name on it. `image` is shown beside the text where given -- the cover,
    /// as encoded bytes -- and the application's icon otherwise.
    void notify(const std::string& title, const std::string& body,
                std::span<const std::byte> image = {});

    /// Takes the icon down now, rather than when this goes: the window closing
    /// and the process ending are not the same moment.
    void remove();

    /// The icon was clicked: show the window.
    std::function<void()> activated;
    /// A row of the menu was chosen, carrying its id.
    std::function<void(int id)> commandChosen;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool add();
    void update();
    void showMenu();

    HWND           hwnd_     = nullptr;
    HICON          icon_     = nullptr;
    HICON          balloon_  = nullptr;
    /// Explorer's "the taskbar was rebuilt" broadcast: the icon is gone and has
    /// to be added again.
    UINT           taskbarCreated_ = 0;
    bool           shown_    = false;
    app::TrayState state_;
};

}  // namespace xpcog::winui
