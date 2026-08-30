// The application's presence outside its own window -- what you reach for when
// the window is buried, minimised, or on another desktop.
//
// That is two different objects on two platforms, deliberately.
//
// Windows and Linux have a notification area, and a media player absent from it
// is one you have to find and raise in order to pause. So they get a tray icon
// carrying a transport menu.
//
// macOS does not get one. Cog contains no NSStatusItem anywhere -- zero matches
// across the whole tree -- and instead hangs its transport off the Dock: a
// `dockMenu` outlet in MainMenu.xib and a DockIconController that badges the
// tile. That is the platform convention, and it is also arithmetic. A menu bar
// extra would be a *second* permanent presence beside a Dock icon the app
// already has, putting the same four commands in two places at once, and the
// Dock icon is the one the user cannot remove.
//
// wx spells both with the same class: wxTaskBarIcon, constructed with wxTBI_DOCK
// on macOS, is the Dock menu. That is a closer fit than Qt managed, where the
// tray was QSystemTrayIcon and the Dock menu was QMenu::setAsDockMenu().
//
// The two attachments do not carry the same items, which is the part that would
// be wrong if this were written as one menu for both. AppKit appends Quit, Show
// All, Options and the window list to a Dock menu itself, and clicking the Dock
// icon already raises the window -- so "Show XPCog" and "Quit" exist only on the
// tray platforms, where nothing else supplies them. Adding them on macOS would
// produce a menu with two Quits.
//
// Every entry in the menu posts a command id the frame already handles, so the
// tray and the menu bar enable and disable together through EVT_UPDATE_UI and
// there is no second play button to keep in step.
//
// --- And then there is Linux ----------------------------------------------
//
// wxTaskBarIcon on GTK is GtkStatusIcon, which is the XEmbed system-tray
// protocol, which is X11. On a Wayland session it cannot work and says so:
// IsAvailable() returns false before it looks at anything. That is most Linux
// desktops now, so the tray platform that wanted this most was the one not
// getting it.
//
// So there are two routes to a tray here, and this class picks. platform::TrayIcon
// is StatusNotifierItem over D-Bus, which no display server is involved in, and
// it is preferred wherever a panel answers for it; wxTaskBarIcon is the fallback,
// unchanged, and still the whole story on Windows. Both are driven from one menu
// model -- menuModel() -- so the two cannot describe different menus, which is
// the failure this arrangement would otherwise invite.
//
// hasTrayIcon() is the answer to "did either of them work", which is what every
// caller actually wanted to ask.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/platform/TrayIcon.hpp"

#include <wx/taskbar.h>
#include <wx/toplevel.h>

#include <memory>
#include <string>
#include <vector>

namespace xpcog::app {

class MainFrame;

/// Un-minimises, raises and focuses `window`.
///
/// Three calls, because none of them alone does the job: a minimised window
/// ignores Raise(), and a raised window belonging to an inactive application does
/// not take focus. Shared with the single-instance handler, which has to do
/// exactly this when a second launch arrives.
void raiseWindow(wxTopLevelWindow* window);

class StatusPresence : public wxTaskBarIcon {
public:
    /// `frame` is the window to raise and where commands are posted; it must
    /// outlive this object, which it does because the frame owns it.
    ///
    /// `dispatch` is what the StatusNotifierItem route needs and the wx route
    /// does not: a panel's click arrives on whichever thread pumps D-Bus, and
    /// this is how it gets back to the one that owns the window.
    StatusPresence(MainFrame* frame, platform::Dispatcher dispatch);

    /// The track now playing, at the top of the menu as in Cog's dock menu.
    /// Either string may be empty -- an unknown artist hides its row rather than
    /// leaving a blank one, which is what Cog does by removing the item.
    void setNowPlaying(const std::string& title, const std::string& artist);

    /// Playing, paused or stopped. Reaches the tooltip, not the icon -- the image
    /// stays the application's, because a tray icon that changes shape is harder
    /// to find than one that does not.
    void setPlaybackState(bool playing, bool paused);

    /// Nothing is playing: drop the track rows and go back to the stopped text.
    void clear();

    /// Whether there is a real tray icon -- not merely *some* presence.
    ///
    /// The distinction matters, and it is not pedantry: on macOS the Dock menu
    /// exists while a tray icon does not. Anything that hides the window and
    /// relies on getting it back has to ask this, or it will hide the window on
    /// macOS and leave nothing to click.
    ///
    /// Which of the two routes produced it is deliberately not exposed. A caller
    /// that branched on it would be encoding today's platform support into a
    /// place that has no business knowing.
    [[nodiscard]] bool hasTrayIcon() const { return hasTrayIcon_; }

    /// A transient notification. `icon` is shown alongside the text where the
    /// platform's notification can carry one; wxNullIcon asks for the default.
    ///
    /// **No tray icon is required, and it used to insist on one.** That guard was
    /// written for the single caller it then had -- the "still playing, in the
    /// tray" notice, which is definitionally about a tray -- and it is that
    /// caller's business rather than this one's, so it now lives there.
    ///
    /// What made the guard look necessary is real but does not have this
    /// consequence: on Windows a notification *is* a taskbar balloon, hung off a
    /// tray icon. wxMSW creates a temporary one when none was supplied
    /// (wxWidgets/src/msw/notifmsg.cpp, wxBalloonNotifMsgImpl::SetUpIcon), so the
    /// notification appears either way. Ours is offered when it exists only so
    /// the balloon comes from the icon already sitting there rather than from one
    /// that blinks into existence to carry it.
    ///
    /// macOS and Linux need no tray at all: wx goes to NSUserNotificationCenter
    /// and to the desktop's notification daemon, neither of which knows what a
    /// tray icon is.
    void notify(const std::string& title, const std::string& body,
                const wxIcon& icon = wxNullIcon);

    // --- wxTaskBarIcon ----------------------------------------------------
    wxMenu* CreatePopupMenu() override;

    /// Overridden so that taking the icon down takes down whichever one is up.
    ///
    /// The frame calls this on the way out and does not know which route was
    /// taken, which is the point -- the alternative was a second method the frame
    /// had to remember to call as well.
    bool RemoveIcon() override;

private:
    /// The menu, once, for both routes to render.
    [[nodiscard]] std::vector<platform::TrayMenuItem> menuModel() const;

    /// Pushes the tooltip and the menu to whichever route is live. Called
    /// whenever the track or the transport state moves, because both appear in
    /// both of them.
    void refresh();

    [[nodiscard]] std::string tooltipBody() const;

    /// Runs the command a row carries, from either route.
    void activateCommand(int id);

    MainFrame* frame_ = nullptr;

    /// StatusNotifierItem, where a panel answers for it. Always constructed --
    /// the base class is a working do-nothing -- and consulted through
    /// isAvailable() rather than through a null check.
    std::unique_ptr<platform::TrayIcon> tray_;
    Subscription                        trayActivated_;
    Subscription                        trayMenuActivated_;

    /// The track, kept because the tooltip is rebuilt from scratch whenever
    /// either the track or the state changes and the two arrive separately.
    std::string title_;
    std::string artist_;

    bool playing_     = false;
    bool paused_      = false;
    bool hasTrayIcon_ = false;
};

}  // namespace xpcog::app
