// Every command the application has, once.
//
// Cog has 225 IBActions spread across XIBs, and every menu item, toolbar button
// and context menu that invokes the same command is a separate connection to it.
// The Qt build answered that with one QAction per command that every surface
// referenced, so enabling a command was one setEnabled() rather than a hunt.
//
// wx has no QAction. A command is an **id** -- an integer that a menu item, a
// button and an accelerator all post as wxEVT_MENU -- and the shared state a
// QAction carried comes from wxEVT_UPDATE_UI instead. That is a better fit than
// it first looks:
//
//   * `Bind(wxEVT_MENU, handler, CommandId::X)` is the connection, and there is
//     still exactly one per command.
//   * `Bind(wxEVT_UPDATE_UI, handler, CommandId::X)` sets the enabled state, the
//     checked state and the label from whatever is true at the time -- and every
//     surface carrying that id gets the answer, so the menu bar, the playlist's
//     context menu and the tray menu cannot drift apart.
//
// Two things that fall out of that and are worth stating, because both were
// awkward under Qt and are not here. Undo and Redo relabel themselves from the
// stack rather than needing a refreshUndoActions() anyone could forget to call.
// And Play/Pause swaps its own icon and label from the transport state, where the
// Qt build had to re-apply the icon after every style change and then put "pause"
// back over the top of what applyIcons() had just written.
//
// The enum is ActionId's, value for value, deliberately. It is effectively the
// whole command surface of the application, and re-deriving it during a toolkit
// port would only introduce differences nobody asked for.

#pragma once

#include <wx/defs.h>

#include <string>
#include <vector>

class wxAcceleratorTable;
class wxMenuBar;
class wxWindow;

namespace xpcog::app {

/// Stable identifiers, so the menu table and the code that handles commands
/// refer to the same thing without a string typo compiling cleanly.
///
/// The standard wx ids are used where one exists, and not for tidiness: on macOS
/// wx relocates wxID_PREFERENCES, wxID_ABOUT and wxID_EXIT into the application
/// menu itself, which is what QAction::setMenuRole() was doing by hand. The rest
/// start above wxID_HIGHEST so they cannot collide with anything the toolkit
/// dispatches on its own.
enum CommandId : int {
    FileOpen         = wxID_OPEN,
    FileSavePlaylist = wxID_SAVEAS,
    FilePreferences  = wxID_PREFERENCES,
    FileQuit         = wxID_EXIT,
    EditUndo         = wxID_UNDO,
    EditRedo         = wxID_REDO,
    EditSelectAll    = wxID_SELECTALL,
    HelpAbout        = wxID_ABOUT,

    FileOpenFolder = wxID_HIGHEST + 1,
    FileOpenUrl,

    EditRemove,
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
    ViewSc55Panel,
    ViewMiniPlayer,

    /// Not a command: the first id a widget in this application may use for
    /// itself, so nothing invents one that collides with the list above.
    FirstWidgetId,
};

/// How a menu item behaves. Radio is a real distinction rather than a
/// presentation one -- consecutive radio items form one exclusive group, and
/// anything between them, **including a separator**, breaks it.
enum class ItemKind { Normal, Check, Radio };

struct MenuItem {
    /// nullptr continues the previous menu.
    const char* menu = nullptr;
    CommandId   id   = FileOpen;
    const char* label = "";
    /// Appended to the label after a tab. wx parses it and builds the
    /// accelerator itself; `Ctrl` becomes Cmd on macOS with no special case.
    const char* accelerator     = "";
    ItemKind    kind            = ItemKind::Normal;
    bool        separatorBefore = false;
};

/// The menu structure, declaratively. Adding a command is one row here plus a
/// Bind, rather than an edit to a XIB whose diff is unreadable.
[[nodiscard]] const std::vector<MenuItem>& menuLayout();

/// Which Lucide glyph each command wears, for the surfaces that draw one.
[[nodiscard]] std::string commandIcon(CommandId id);

/// The transport buttons, in order.
[[nodiscard]] const std::vector<CommandId>& transportLayout();

/// Builds the whole menu bar from the table above.
[[nodiscard]] wxMenuBar* buildMenuBar();

}  // namespace xpcog::app
