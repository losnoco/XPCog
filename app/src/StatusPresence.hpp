// The application's presence outside its own window -- what you reach for when
// the window is buried, minimised, or on another desktop.
//
// That is two different objects on two platforms, deliberately.
//
// Windows and Linux have a notification area, and a media player absent from it
// is one you have to find and raise in order to pause. So they get a
// QSystemTrayIcon carrying a transport menu.
//
// macOS does not get one. Cog contains no NSStatusItem anywhere -- zero matches
// across the whole tree -- and instead hangs its transport off the Dock: a
// `dockMenu` outlet in MainMenu.xib and a DockIconController that badges the
// tile. That is the platform convention, and it is also arithmetic. A menu bar
// extra would be a *second* permanent presence beside a Dock icon the app
// already has, putting the same four commands in two places at once, and the
// Dock icon is the one the user cannot remove.
//
// So the menu is built once and attached differently: to a tray icon off macOS,
// to the Dock tile on it, through QMenu::setAsDockMenu().
//
// The two attachments do not carry the same items, which is the part that would
// be wrong if this were written as one menu for both. AppKit appends Quit, Show
// All, Options and the window list to a Dock menu itself, and clicking the Dock
// icon already raises the window -- so "Show XPCog" and "Quit" exist only on the
// tray platforms, where nothing else supplies them. Adding them on macOS would
// produce a menu with two Quits.
//
// Every command in the menu is a QAction the registry already owns, so the tray
// and the menu bar enable and disable together and there is no second play
// button to keep in step.

#pragma once

#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;
class QWidget;

namespace xpcog::app {

class ActionRegistry;

/// Un-minimises, raises and focuses `window`.
///
/// Three calls, because none of them alone does the job: a minimised window
/// ignores raise(), and a raised window belonging to an inactive application
/// does not take focus. Shared with the single-instance handler, which has to
/// do exactly this when a second launch arrives.
void raiseWindow(QWidget* window);

class StatusPresence : public QObject {
    Q_OBJECT

public:
    /// `window` is the window to raise, and the menu's parent; it must outlive
    /// this object, which it does because the window owns it.
    StatusPresence(const ActionRegistry& actions, QWidget* window,
                   QObject* parent = nullptr);

    /// The track now playing, at the top of the menu as in Cog's dock menu.
    /// Either string may be empty -- an unknown artist hides its row rather
    /// than leaving a blank one, which is what Cog does by removing the item.
    void setNowPlaying(const QString& title, const QString& artist);

    /// Drives the tray icon's glyph. Cog badges its Dock tile with play, pause
    /// or stop; a tray icon is the same idea with the only image the app has.
    void setPlaybackState(bool playing, bool paused);

    /// Nothing is playing: drop the track rows and go back to the stopped glyph.
    void clear();

    /// False when there is no presence at all -- a Linux session with no
    /// notification area. Exposed so a caller can tell "no tray" from "tray that
    /// does nothing" rather than wondering why its updates go nowhere.
    [[nodiscard]] bool isVisible() const { return tray_ != nullptr || menu_ != nullptr; }

private:
    void refreshIcon();

    QWidget* window_ = nullptr;
    QMenu*   menu_   = nullptr;

    /// Null on macOS, and on any system with no notification area.
    QSystemTrayIcon* tray_ = nullptr;

    /// The two disabled rows at the top, plus the separator below them. Held so
    /// they can be hidden as a group when nothing is playing.
    QAction* artistRow_ = nullptr;
    QAction* titleRow_  = nullptr;
    QAction* infoSplit_ = nullptr;

    bool playing_ = false;
    bool paused_  = false;
};

}  // namespace xpcog::app
