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

#include <string>
#include <string_view>
#include <vector>

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
    // Written out rather than taken from <wx/defs.h>, because this header names
    // no toolkit -- and checked rather than trusted: app/src/CommandIds.cpp is a
    // file of nothing but static_asserts against wx's own values, so a wx that
    // renumbered these would fail to build on all three platforms rather than
    // relocating the wrong menu items on one.
    //
    // They are wx's numbering and not an arbitrary base, because the relocation
    // above keys on the *item's own id*. A neutral enum plus a translation table
    // would mean mapping on the way into every menu and back out of every
    // wxEVT_MENU, which is exactly the "one id, one Bind" property this enum
    // exists to have.
    FileOpen         = 5000,  // wxID_OPEN
    FileSavePlaylist = 5004,  // wxID_SAVEAS
    FilePreferences  = 5022,  // wxID_PREFERENCES
    FileQuit         = 5006,  // wxID_EXIT
    EditUndo         = 5007,  // wxID_UNDO
    EditRedo         = 5008,  // wxID_REDO
    EditSelectAll    = 5037,  // wxID_SELECTALL
    HelpAbout        = 5014,  // wxID_ABOUT

    // Above every id wx reserves, in both of the versions it has meant: wx 3.2
    // defines wxID_HIGHEST as 5999, the highest reserved id, and 3.3 redefined
    // it as 6000, the lowest unreserved one. 6001 clears both, which is what
    // CommandIds.cpp asserts against whichever wx the Windows build has.
    FileOpenFolder = 6001,
    FileOpenUrl,

    EditRemove,
    EditRandomize,
    EditScrollToCurrent,

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
    ViewOscilloscope,
    ViewWaveform,
    ViewEqualizer,
    ViewSpeed,
    ViewInfo,
    ViewLyrics,
    ViewTimedLyrics,
    ViewFollowSelection,
    ViewFollowPlayback,
    ViewSc55Panel,
    ViewDockPanes,
    ViewMiniPlayer,

    // The playlist's context menu. Cog reaches most of these from its Playlist
    // menu as well; here they live on the selection they act on, because that is
    // where the question "what about *these* tracks" is asked. EditRemove and
    // ViewInfo appear on the same menu and are not repeated -- one command, one
    // id, however many surfaces carry it.
    PlaylistToggleQueued,
    PlaylistStopAfter,
    PlaylistSaveSelection,
    PlaylistSearchArtist,
    PlaylistSearchAlbum,
    PlaylistReloadInfo,
    PlaylistResetPlayCount,
    PlaylistRemoveRating,
    PlaylistReveal,
    PlaylistTrash,

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

/// The playlist's context menu, in Cog's order.
///
/// The same table shape, and deliberately: these rows carry the same ids the
/// menu bar does, so the enabled state, the ticks and the two labels that
/// rewrite themselves all arrive from the one EVT_UPDATE_UI handler each command
/// already has. `menu` is unused here -- a popup has no title.
[[nodiscard]] const std::vector<MenuItem>& playlistMenuLayout();

/// Which Lucide glyph each command wears, for the surfaces that draw one.
[[nodiscard]] std::string commandIcon(CommandId id);

/// The transport buttons, in order.
[[nodiscard]] const std::vector<CommandId>& transportLayout();

/// One tool on the main window's toolbar. A subset of MenuItem, because a
/// toolbar has no submenu structure and takes its wording from the menu row
/// carrying the same id rather than repeating it.
struct ToolbarItem {
    CommandId id   = PlaybackPlayPause;
    /// Check draws a tool that stays pressed while its pane is open, and gets
    /// its state from the same EVT_UPDATE_UI handler the menu tick does.
    ItemKind kind            = ItemKind::Normal;
    bool     separatorBefore = false;
};

/// The main window's toolbar: the transport, and nothing else.
///
/// Built from transportLayout() rather than repeating it, so the order of the
/// transport is stated once and the mini player cannot drift from the toolbar.
[[nodiscard]] const std::vector<ToolbarItem>& toolbarLayout();

/// The command's menu label, translated, with the mnemonic ampersands removed.
///
/// Read out of menuLayout(), so a toolbar tool and a menu item cannot end up
/// named differently -- and a command with no menu row returns an empty string
/// rather than inventing one.
[[nodiscard]] std::string commandLabel(CommandId id);

/// The command's accelerator as this table spells it -- "Ctrl+Right" -- or
/// empty.
///
/// Rendered by whoever is drawing it, because the answer is not the same
/// everywhere: wx turns `Ctrl` into Cmd and draws macOS's own modifier symbols,
/// and a surface that paints its own menus writes the literal into the shortcut
/// column. See commandTooltip() in WxMenus.hpp for the wx side of that.
[[nodiscard]] std::string_view commandAccelerator(CommandId id);

/// A command as an action name, for a surface that addresses commands by name
/// rather than by integer -- GAction, where a menu item and a shortcut both
/// name "win.play-pause" and a stateful group is one action with a target.
///
/// `name` is kebab-case and stable: it appears in .ui files and in saved
/// shortcuts, so renaming one is a compatibility change. `target` is set for
/// the radio groups only -- repeat, shuffle, panels-follow -- where the four
/// or three commands are one action whose state is the target string, and is
/// null for everything else.
struct CommandAction {
    const char* name;
    const char* target;
};

/// The action for `id`. Every CommandId has one; tests/uicore/test_commands
/// walks the table and insists on it, so a command added later cannot arrive
/// with no way to be invoked from the frontend that uses names.
[[nodiscard]] CommandAction commandAction(CommandId id);

/// Every command, in the enum's order, for the tests that walk them.
[[nodiscard]] const std::vector<CommandId>& allCommands();

/// `label` with its mnemonic markers taken out: a lone `&` disappears and marks
/// the character after it, and `&&` is a literal ampersand.
///
/// What wxControl::RemoveMnemonics does. Spelled out because this layer has no
/// toolkit to ask, and asserted against wx's own answer for every row of every
/// table in tests/app/test_commands.cpp.
[[nodiscard]] std::string stripMnemonics(std::string_view label);

}  // namespace xpcog::app
