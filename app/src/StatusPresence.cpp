#include "StatusPresence.hpp"

#include "ActionRegistry.hpp"
#include "AppIcon.hpp"

#include <QAction>
#include <QFontMetrics>
#include <QIcon>
#include <QMenu>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QWidget>

namespace xpcog::app {
namespace {

/// The commands, in Cog's dock-menu order (MainMenu.xib, menu 513): transport
/// first, then track movement. Cog also carries Stop After Current and the
/// album skips, which XPCog has no command for yet -- when it does, they belong
/// here rather than in a second list.
constexpr ActionId kTransport[] = {
    ActionId::PlaybackPlayPause,
    ActionId::PlaybackStop,
};

constexpr ActionId kNavigation[] = {
    ActionId::PlaybackPrevious,
    ActionId::PlaybackNext,
};

/// A long title would otherwise stretch the menu to the width of the screen,
/// and a tray menu has no other content to hold it in shape.
QString elide(const QString& text, const QFontMetrics& metrics) {
    constexpr int kMaxPixels = 320;
    return metrics.elidedText(text, Qt::ElideRight, kMaxPixels);
}

}  // namespace

void raiseWindow(QWidget* window) {
    if (window == nullptr) {
        return;
    }
    window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
}

StatusPresence::StatusPresence(const ActionRegistry& actions, QWidget* window,
                               QObject* parent)
    : QObject(parent), window_(window) {
    // On the tray platforms, ask first: a Linux session may have no notification
    // area at all, and QSystemTrayIcon::show() on one of those is a silent
    // no-op that would leave a menu attached to nothing.
#ifndef Q_OS_MACOS
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
#endif

    menu_ = new QMenu(window);

    // Two disabled rows for the track, as Cog's dock menu opens with the artist
    // and the song. Created up front and hidden rather than inserted on demand:
    // Cog inserts and removes its artist item by index, which is why that code
    // has to ask where the item currently is before touching it.
    artistRow_ = menu_->addAction(QString{});
    artistRow_->setEnabled(false);
    titleRow_ = menu_->addAction(QString{});
    titleRow_->setEnabled(false);
    infoSplit_ = menu_->addSeparator();

    for (const ActionId id : kTransport) {
        if (QAction* command = actions.action(id); command != nullptr) {
            menu_->addAction(command);
        }
    }
    menu_->addSeparator();
    for (const ActionId id : kNavigation) {
        if (QAction* command = actions.action(id); command != nullptr) {
            menu_->addAction(command);
        }
    }

    clear();

#ifdef Q_OS_MACOS
    // The Dock tile's menu. AppKit adds Quit, Show All, Options and the window
    // list below whatever is here, so this menu stops at the transport.
    menu_->setAsDockMenu();
#else
    // Raising the window and quitting exist only here: on macOS the Dock icon
    // does the first and AppKit's own menu does the second.
    menu_->addSeparator();
    QAction* show = menu_->addAction(tr("Show XPCog"));
    connect(show, &QAction::triggered, this, [this] { raiseWindow(window_); });
    if (QAction* quit = actions.action(ActionId::FileQuit); quit != nullptr) {
        menu_->addAction(quit);
    }

    tray_ = new QSystemTrayIcon(this);
    tray_->setContextMenu(menu_);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                // Trigger is a single click, which is the Windows convention;
                // DoubleClick is what several Linux shells send instead. Context
                // is the right-click that opens the menu and must not also raise
                // the window out from under it.
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    raiseWindow(window_);
                }
            });
    tray_->setIcon(applicationIcon());
    refreshTooltip();
    tray_->show();
#endif
}

void StatusPresence::setNowPlaying(const QString& title, const QString& artist) {
    if (menu_ == nullptr) {
        return;
    }

    const QFontMetrics metrics(menu_->font());
    titleRow_->setText(elide(title, metrics));
    titleRow_->setVisible(!title.isEmpty());
    artistRow_->setText(elide(artist, metrics));
    artistRow_->setVisible(!artist.isEmpty());
    infoSplit_->setVisible(!title.isEmpty() || !artist.isEmpty());

    title_  = title;
    artist_ = artist;
    refreshTooltip();
}

void StatusPresence::setPlaybackState(bool playing, bool paused) {
    playing_ = playing;
    paused_  = paused;
    refreshTooltip();
}

void StatusPresence::clear() {
    playing_ = false;
    paused_  = false;
    setNowPlaying(QString{}, QString{});
}

void StatusPresence::showMessage(const QString& title, const QString& body) {
    if (tray_ == nullptr) {
        return;
    }
    tray_->showMessage(title, body, applicationIcon());
}

void StatusPresence::refreshTooltip() {
    if (tray_ == nullptr) {
        return;
    }

    // The tray keeps the application icon and puts the state in the tooltip,
    // rather than swapping the image for a play or pause glyph.
    //
    // Cog does swap -- it badges the Dock tile -- but a Dock tile is 128px and a
    // tray icon is 16, where a corner badge is three or four pixels and reads as
    // dirt. It also costs the only thing the tray is actually for, which is
    // being recognisable at a glance among a row of other icons.
    //
    // Badging belongs to the taskbar/Dock progress work (NSDockTile,
    // ITaskbarList3), which has a real API for it at a size that can carry it.
    const QString what = artist_.isEmpty() ? title_
                         : title_.isEmpty()
                             ? artist_
                             : QStringLiteral("%1 — %2").arg(artist_, title_);

    QString state;
    if (playing_ && paused_) {
        state = tr("Paused");
    } else if (playing_) {
        state = tr("Playing");
    }

    QStringList lines;
    lines.append(QStringLiteral("XPCog"));
    if (!state.isEmpty()) {
        lines.append(state);
    }
    if (!what.isEmpty()) {
        lines.append(what);
    }
    tray_->setToolTip(lines.join(QStringLiteral(" — ")));
}

}  // namespace xpcog::app
