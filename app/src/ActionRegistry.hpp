// One QAction per command, created once.
//
// Cog has 225 IBActions spread across XIBs, and every menu item, toolbar button
// and context menu that invokes the same command is a separate connection to it.
// Here each command exists exactly once and menus, toolbars, context menus,
// shortcuts and (later) media keys all reference the same QAction -- so
// enabling or disabling a command is one setEnabled() rather than a hunt.

#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>

class QAction;
class QMenuBar;
class QToolBar;

namespace xpcog::app {

/// Stable identifiers, so the menu table and the code that enables commands
/// refer to the same thing without a string typo compiling cleanly.
enum class ActionId {
    FileOpen,
    FileOpenFolder,
    FileSavePlaylist,
    FilePreferences,
    FileQuit,

    EditRemove,
    EditSelectAll,

    PlaybackPlayPause,
    PlaybackStop,
    PlaybackNext,
    PlaybackPrevious,
    PlaybackEnqueue,

    OrderRepeatNone,
    OrderRepeatOne,
    OrderRepeatAlbum,
    OrderRepeatAll,
    OrderShuffleOff,
    OrderShuffleAlbums,
    OrderShuffleAll,

    ViewFileTree,
};

class ActionRegistry : public QObject {
    Q_OBJECT

public:
    explicit ActionRegistry(QObject* parent = nullptr);

    [[nodiscard]] QAction* action(ActionId id) const;

    /// Builds the menu bar from a declarative table. ~250 lines of table
    /// replacing 158 hand-maintained menu items of XML.
    void populateMenuBar(QMenuBar* bar) const;
    void populateToolBar(QToolBar* bar) const;

private:
    QAction* add(ActionId id, const QString& text, const QKeySequence& shortcut = {});

    QHash<int, QAction*> actions_;
};

}  // namespace xpcog::app
