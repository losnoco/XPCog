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

#pragma once

#include <wx/taskbar.h>
#include <wx/toplevel.h>

#include <string>

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
    explicit StatusPresence(MainFrame* frame);

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

private:
    void refreshTooltip();

    MainFrame* frame_ = nullptr;

    /// The track, kept because the tooltip is rebuilt from scratch whenever
    /// either the track or the state changes and the two arrive separately.
    std::string title_;
    std::string artist_;

    bool playing_     = false;
    bool paused_      = false;
    bool hasTrayIcon_ = false;
};

}  // namespace xpcog::app
