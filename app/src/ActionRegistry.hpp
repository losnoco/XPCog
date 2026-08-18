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
    FileOpenUrl,
    FileSavePlaylist,
    FilePreferences,
    FileQuit,

    EditUndo,
    EditRedo,
    EditRemove,
    EditSelectAll,
    EditRandomize,

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
    ViewFileTreeRoot,
    ViewSpectrum,
    ViewEqualizer,
    ViewInfo,
    ViewMiniPlayer,

    HelpAbout,
    HelpAboutQt,
};

class ActionRegistry : public QObject {
    Q_OBJECT

public:
    explicit ActionRegistry(QObject* parent = nullptr);

    [[nodiscard]] QAction* action(ActionId id) const;

    /// Re-renders every command's icon in the current palette's colour.
    ///
    /// Called at construction and again whenever the style changes.
    /// QApplication::setStyle() re-polishes every widget and resets the palette,
    /// so icons rendered for the previous one are left stroked in a colour that
    /// may now be the colour of the surface behind them -- which is how switching
    /// to windowsvista, whose chrome stays light, made a dark theme's near-white
    /// icons invisible.
    void applyIcons();

    /// Builds the menu bar from a declarative table. ~250 lines of table
    /// replacing 158 hand-maintained menu items of XML.
    void populateMenuBar(QMenuBar* bar) const;
    void populateToolBar(QToolBar* bar) const;

private:
    QAction* add(ActionId id, const QString& text, const QKeySequence& shortcut = {});

    QHash<int, QAction*> actions_;
};

}  // namespace xpcog::app
