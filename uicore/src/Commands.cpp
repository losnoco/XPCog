#include "Commands.hpp"

#include "Translations.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace xpcog::app {


namespace {

// Two labels that name something the platform names differently, so the
// literal is chosen at compile time. Both spellings still reach the catalogue:
// the extractor reads the source rather than the preprocessor's output, so a
// translator sees every branch whichever platform they are on.
//
// **`_WIN32`, not `__WXMSW__`.** That comes from wx/platform.h, and this file
// includes no wx -- so it would be undefined here, and Windows would quietly
// get the Linux wording. Nothing would fail to compile and no test would
// notice.
#if defined(_WIN32)
constexpr const char* kRevealLabel = XPCOG_TRANSLATE("Show in &Explorer");
#else
constexpr const char* kRevealLabel = XPCOG_TRANSLATE("Show in &File Manager");
#endif

#if defined(_WIN32)
constexpr const char* kTrashLabel = XPCOG_TRANSLATE("Move to the Recycle &Bin");
#else
constexpr const char* kTrashLabel = XPCOG_TRANSLATE("Move to &Trash");
#endif

// The accelerators, and where they differ from Cog:
//
//   Open URL is Ctrl-L rather than Cog's Cmd-Shift-O. Cog has no separate Open
//   Folder command -- its open panel takes directories -- so Ctrl-Shift-O is
//   free there and taken here. Ctrl-L is what a browser or VLC uses for the same
//   "open a location" idea.
//
//   Play/Pause is deliberately not given a plain Space accelerator. wx builds a
//   real accelerator table from these, and a bare Space would be swallowed
//   before the playlist's filter box ever saw it -- which is a latent problem in
//   the Qt build and a loud one here. The frame handles Space itself, on the
//   playlist, where it cannot steal from a text field.

const std::vector<MenuItem>& layout() {
    static const std::vector<MenuItem> table = {
        {XPCOG_TRANSLATE("&File"), FileOpen, XPCOG_TRANSLATE("&Open Files..."), "Ctrl+O"},
        {nullptr, FileOpenFolder, XPCOG_TRANSLATE("Open &Folder..."), "Ctrl+Shift+O"},
        {nullptr, FileOpenUrl, XPCOG_TRANSLATE("Open &URL..."), "Ctrl+L"},
        // No accelerator, deliberately: this is a thing somebody does once, when
        // they move over from Cog, and a shortcut for it would be occupying a
        // key for the rest of the installation's life.
        {nullptr, FileSavePlaylist, XPCOG_TRANSLATE("&Save Playlist..."), "Ctrl+S", ItemKind::Normal, true},
        {nullptr, FilePreferences, XPCOG_TRANSLATE("&Preferences..."), "Ctrl+,", ItemKind::Normal, true},
        {nullptr, FileQuit, XPCOG_TRANSLATE("&Quit"), "Ctrl+Q", ItemKind::Normal, true},

        {XPCOG_TRANSLATE("&View"), ViewFileTree, XPCOG_TRANSLATE("&File Browser"), "Ctrl+B", ItemKind::Check},
        // Not checkable: it opens a dialog rather than showing or hiding
        // anything. Directly under the browser it belongs to, because a root you
        // cannot find is a browser stuck wherever it was left.
        {nullptr, ViewFileTreeRoot, XPCOG_TRANSLATE("Choose &Root Folder..."), ""},
        // Cog's Info Inspector, on Cog's shortcut.
        {nullptr, ViewInfo, XPCOG_TRANSLATE("&Info"), "Ctrl+I", ItemKind::Check, true},
        // Cog's Lyrics window, on Cog's shortcut and directly under Info, which
        // is where Cog groups it too. Cog labels it "Show Lyrics"; the items in
        // this menu are checkable toggles that say what they show rather than
        // what they do, so it is "&Lyrics" here. Ctrl+L is Open URL, so the
        // shifted form is not a second choice -- it is Cog's own (Cmd+Shift+L).
        {nullptr, ViewLyrics, XPCOG_TRANSLATE("&Lyrics"), "Ctrl+Shift+L", ItemKind::Check},
        // How that pane shows timed lyrics: followed, or as plain text. Under
        // the pane it governs, and above the radio pair rather than inside it,
        // since anything between those two breaks their group.
        {nullptr, ViewTimedLyrics, XPCOG_TRANSLATE("&Timed Lyrics"), "", ItemKind::Check},
        // Which track those two describe. A radio pair rather than one checkable
        // item, because "Follow Playback" unticked does not say what it does
        // instead -- and what it does instead is not "nothing", it is the other
        // mode.
        //
        // Deliberately *not* separated from Info and Lyrics above: it governs
        // exactly those two and nothing else, and a separator here would file it
        // with Spectrum and the Equalizer, which it has nothing to do with. The
        // separator moves down to Spectrum instead, which is where the subject
        // actually changes.
        //
        // Consecutive radio items form one exclusive group and anything between
        // them breaks it -- including a separator -- so nothing may be inserted
        // between these two.
        {nullptr, ViewFollowSelection, XPCOG_TRANSLATE("Panels Follow &Selection"), "", ItemKind::Radio},
        {nullptr, ViewFollowPlayback, XPCOG_TRANSLATE("Panels Follow Play&back"), "", ItemKind::Radio},
        {nullptr, ViewSpectrum, XPCOG_TRANSLATE("&Spectrum"), "Ctrl+U", ItemKind::Check, true},
        // The other visualiser. No shortcut: U is the spectrum's, and one key
        // for the pane most people open once is enough.
        {nullptr, ViewOscilloscope, XPCOG_TRANSLATE("&Oscilloscope"), "", ItemKind::Check},
        // The seek bar's waveform: on, the transport is taller and shows the
        // track's shape; off, it is the plain bar it always was. Here as well
        // as on the Appearance pane, which holds the two drawing choices too,
        // because it is looked at rather than configured.
        {nullptr, ViewWaveform, XPCOG_TRANSLATE("Show &Waveform"), "", ItemKind::Check},
        // Cog keeps its equaliser in a window of its own rather than in
        // preferences, and so does this. Ctrl-E, which nothing else claims.
        {nullptr, ViewEqualizer, XPCOG_TRANSLATE("&Equalizer"), "Ctrl+E", ItemKind::Check},
        // Pitch and tempo, which Cog reaches from a toolbar button and a
        // popover. No shortcut: the keys worth spending are spent, and this is
        // a pane somebody opens once in a session and leaves open.
        {nullptr, ViewSpeed, XPCOG_TRANSLATE("&Pitch && Tempo"), "", ItemKind::Check},
        // The SC-55's front panel. No shortcut: it is worth having and it is not
        // worth a key -- one synthesiser of three, for one format. Present even
        // in a build without MIDI, where it simply never finds a pane to toggle;
        // a command that appears and disappears with a compile flag is worse
        // than one that is occasionally inert.
        {nullptr, ViewSc55Panel, XPCOG_TRANSLATE("SC-55 &Panel"), "", ItemKind::Check},
        // Putting a torn-off pane back, which on Wayland is otherwise
        // impossible: docking one is a drag, and a Wayland client cannot place
        // its own surfaces, so wxAUI never sees the gesture that would re-dock
        // it. A menu item is the only way back, and it costs nothing on the two
        // platforms where the drag works.
        //
        // Disabled when nothing is floating, so it also answers "is anything
        // torn off" rather than sitting there permanently clickable.
        {nullptr, ViewDockPanes, XPCOG_TRANSLATE("&Dock Floating Panes"), "", ItemKind::Normal, true},
        {nullptr, ViewMiniPlayer, XPCOG_TRANSLATE("&Mini Player"), "Ctrl+M", ItemKind::Check, true},

        {XPCOG_TRANSLATE("&Edit"), EditUndo, XPCOG_TRANSLATE("&Undo"), "Ctrl+Z"},
        {nullptr, EditRedo, XPCOG_TRANSLATE("&Redo"), "Ctrl+Y"},
        {nullptr, EditSelectAll, XPCOG_TRANSLATE("Select &All"), "Ctrl+A", ItemKind::Normal, true},
        // Cog's "Select Currently Playing", directly under Select All where Cog
        // keeps it, and doing what Cog's does: -scrollToCurrentEntry: scrolls
        // the row into view *and* selects it, replacing the selection rather
        // than extending it. The name describes the selection; the scroll is
        // what it is reached for, after ten minutes of looking through a long
        // playlist for where you are.
        //
        // Ctrl-J rather than Cog's Cmd-L, which is Open URL here -- see the
        // accelerator note at the top of this file. J is what several players
        // use for jumping to the playing track, and nothing here claims it.
        {nullptr, EditScrollToCurrent, XPCOG_TRANSLATE("Select Currently &Playing"), "Ctrl+J"},
        {nullptr, EditRemove, XPCOG_TRANSLATE("&Remove from Playlist"), "Del"},
        {nullptr, EditRandomize, XPCOG_TRANSLATE("Randomi&ze Playlist"), "", ItemKind::Normal, true},

        {XPCOG_TRANSLATE("&Playback"), PlaybackPlayPause, XPCOG_TRANSLATE("&Play/Pause"), ""},
        {nullptr, PlaybackStop, XPCOG_TRANSLATE("&Stop"), "Ctrl+."},
        {nullptr, PlaybackPrevious, XPCOG_TRANSLATE("Pre&vious"), "Ctrl+Left", ItemKind::Normal, true},
        {nullptr, PlaybackNext, XPCOG_TRANSLATE("&Next"), "Ctrl+Right"},
        {nullptr, PlaybackEnqueue, XPCOG_TRANSLATE("Add to &Queue"), "Q", ItemKind::Normal, true},

        // Two exclusive sets. Cog drives these from an NSPopUpButton plus four
        // NSValueTransformers; consecutive radio items are the same idea with
        // the transformers deleted.
        //
        // The separator between the Repeat four and the Shuffle three is
        // load-bearing and must not be removed: a separator breaks a radio run,
        // which is exactly what keeps these two groups from becoming one.
        {XPCOG_TRANSLATE("&Order"), OrderRepeatNone, XPCOG_TRANSLATE("Repeat: &Off"), "", ItemKind::Radio},
        {nullptr, OrderRepeatOne, XPCOG_TRANSLATE("Repeat: &One"), "", ItemKind::Radio},
        {nullptr, OrderRepeatAlbum, XPCOG_TRANSLATE("Repeat: &Album"), "", ItemKind::Radio},
        {nullptr, OrderRepeatAll, XPCOG_TRANSLATE("Repeat: A&ll"), "", ItemKind::Radio},
        {nullptr, OrderShuffleOff, XPCOG_TRANSLATE("Shuffle: O&ff"), "", ItemKind::Radio, true},
        {nullptr, OrderShuffleAlbums, XPCOG_TRANSLATE("Shuffle: Al&bums"), "", ItemKind::Radio},
        {nullptr, OrderShuffleAll, XPCOG_TRANSLATE("Shuffle: A&ll Tracks"), "", ItemKind::Radio},

        // No "About Qt" any more, and nothing replaces it: the toolkit's version
        // is a row in the About box's component list, which is where it belongs.
        {XPCOG_TRANSLATE("&Help"), HelpAbout, XPCOG_TRANSLATE("&About XPCog"), ""},
    };
    return table;
}

// The playlist's context menu: Cog's ContextualMenu (MainMenu.xib:2606), row for
// row and in its order, with three deliberate differences.
//
//   * "Information" is dropped. It is hidden in Cog's own XIB and connected to
//     nothing.
//   * "Properties" is the Info pane rather than a window of its own, so it is
//     ViewInfo -- the same id the View menu carries, checkable, ticked while the
//     pane is open. Cog's item opens its Info Inspector; a second command that
//     only ever showed the pane the View menu toggles would be two ways to say
//     one thing, and the tick answers "is it already open" which Cog's does not.
//   * Remove keeps the Edit menu's label and its Del accelerator, rather than
//     Cog's bare "Remove". The accelerator is displayed here, not created --
//     wx builds an accelerator table from the menu *bar*, and a popup only draws
//     what it is given.
const std::vector<MenuItem>& playlistLayout() {
    static const std::vector<MenuItem> table = {
        // Relabelled from the selection: "Add to Queue", "Remove from Queue", or
        // "Toggle Queued" when the selection is mixed. Cog does this with an
        // NSValueTransformer bound to selection.queued; here it is three lines in
        // the EVT_UPDATE_UI handler.
        {nullptr, PlaylistToggleQueued, XPCOG_TRANSLATE("Add to &Queue"), ""},
        {nullptr, PlaylistStopAfter, XPCOG_TRANSLATE("Stop after &Selection"), ""},
        {nullptr, PlaylistSaveSelection, XPCOG_TRANSLATE("Save Selection as &Playlist..."),
         "", ItemKind::Normal, true},
        {nullptr, PlaylistSearchArtist, XPCOG_TRANSLATE("Search for &Artist"), "",
         ItemKind::Normal, true},
        {nullptr, PlaylistSearchAlbum, XPCOG_TRANSLATE("Search for Al&bum"), ""},
        {nullptr, PlaylistReloadInfo, XPCOG_TRANSLATE("Re&load Info"), "", ItemKind::Normal,
         true},
        {nullptr, PlaylistResetPlayCount, XPCOG_TRANSLATE("Reset Play &Count"), ""},
        {nullptr, PlaylistRemoveRating, XPCOG_TRANSLATE("Remove Ra&ting"), ""},
        {nullptr, PlaylistReveal, kRevealLabel, ""},
        {nullptr, EditRemove, XPCOG_TRANSLATE("&Remove from Playlist"), "Del",
         ItemKind::Normal, true},
        {nullptr, PlaylistTrash, kTrashLabel, ""},
        {nullptr, ViewInfo, XPCOG_TRANSLATE("&Info"), "", ItemKind::Check, true},
    };
    return table;
}

/// A table rather than a call beside each command, because these have to be
/// re-applied whenever the system appearance changes -- and a refresh that walks
/// a list cannot forget one, where a refresh repeating scattered calls will.
const std::map<CommandId, std::string>& icons() {
    static const std::map<CommandId, std::string> table = {
        // Play/Pause wears "play" here and is swapped to "pause" from the
        // transport state. Under Qt that swap had to happen *after* the
        // icon refresh, because the refresh would put "play" back; EVT_UPDATE_UI
        // sets both from state every idle, so the ordering hazard is gone.
        {PlaybackPlayPause, "play"},
        {PlaybackStop, "square"},
        {PlaybackNext, "skip-forward"},
        {PlaybackPrevious, "skip-back"},

        {ViewFileTree, "panel-left"},
        {ViewFileTreeRoot, "folder-open"},
        {ViewInfo, "info"},
        {ViewSpectrum, "audio-lines"},
        {ViewEqualizer, "sliders-vertical"},
    };
    return table;
}

/// One row, onto whichever menu is being built.

/// The menu bar's row for a command, or nullptr where it has none.
///
/// The menu bar rather than the playlist's context menu: a command on both
/// carries the real accelerator in the first and a display-only one in the
/// second, and a tooltip promising a key that does nothing is worse than one
/// promising nothing.
[[nodiscard]] const MenuItem* findMenuItem(CommandId id) {
    for (const MenuItem& item : layout()) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

const std::vector<MenuItem>& menuLayout() { return layout(); }

const std::vector<MenuItem>& playlistMenuLayout() { return playlistLayout(); }

std::string commandIcon(CommandId id) {
    const auto found = icons().find(id);
    return found != icons().end() ? found->second : std::string{};
}

const std::vector<CommandId>& transportLayout() {
    static const std::vector<CommandId> table = {
        PlaybackPrevious,
        PlaybackPlayPause,
        PlaybackStop,
        PlaybackNext,
    };
    return table;
}

const std::vector<ToolbarItem>& toolbarLayout() {
    static const std::vector<ToolbarItem> table = [] {
        std::vector<ToolbarItem> rows;

        // The transport, and nothing else.
        //
        // It used to carry check tools for the file browser, Info, Spectrum and
        // the equaliser as well. They are panes: they stay open once opened, so
        // each button was earning its width about once a session, and a toolbar
        // that lists every pane is a second View menu drawn wider. All four are
        // still in the View menu with the accelerators they always had.
        //
        // What is left is what somebody reaches for *while* listening, which is
        // also why there is no separator any more -- there is one group now, and
        // a divider needs two.
        for (const CommandId id : transportLayout()) {
            rows.push_back({id, ItemKind::Normal, false});
        }

        return rows;
    }();
    return table;
}


std::string stripMnemonics(std::string_view label) {
    // What wxControl::RemoveMnemonics does, spelled out because there is no wx
    // here to ask. Two rules, and the second is the one a hand-rolled version
    // gets wrong: a lone `&` marks the next character as the underlined one and
    // disappears, and `&&` is a literal ampersand -- which this table needs, for
    // "&Pitch && Tempo".
    std::string out;
    out.reserve(label.size());
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] != '&') {
            out.push_back(label[i]);
            continue;
        }
        if (i + 1 < label.size() && label[i + 1] == '&') {
            out.push_back('&');
            ++i;
        }
        // A trailing lone '&' is dropped, which is what wx does with it too.
    }
    return out;
}

std::string commandLabel(CommandId id) {
    const MenuItem* item = findMenuItem(id);
    if (item == nullptr) {
        return {};
    }
    // The ampersand is a menu's underline marker and nothing else's. A tooltip
    // reading "&Next" is the one place this table's wording escapes the menu
    // without the toolkit stripping it on the way.
    return stripMnemonics(tr(item->label));
}

std::string_view commandAccelerator(CommandId id) {
    const MenuItem* item = findMenuItem(id);
    return item == nullptr ? std::string_view{} : std::string_view{item->accelerator};
}

const std::vector<CommandId>& allCommands() {
    static const std::vector<CommandId> table = {
        FileOpen,          FileSavePlaylist,     FilePreferences,      FileQuit,
        EditUndo,          EditRedo,             EditSelectAll,        HelpAbout,
        FileOpenFolder,    FileOpenUrl,          EditRemove,
        EditRandomize,     EditScrollToCurrent,  PlaybackPlayPause,    PlaybackStop,
        PlaybackNext,      PlaybackPrevious,     PlaybackEnqueue,      OrderRepeatNone,
        OrderRepeatOne,    OrderRepeatAlbum,     OrderRepeatAll,       OrderShuffleOff,
        OrderShuffleAlbums, OrderShuffleAll,     ViewFileTree,         ViewFileTreeRoot,
        ViewSpectrum,      ViewOscilloscope,     ViewWaveform,         ViewEqualizer,
        ViewSpeed,         ViewInfo,             ViewLyrics,           ViewTimedLyrics,
        ViewFollowSelection,
        ViewFollowPlayback, ViewSc55Panel,       ViewDockPanes,        ViewMiniPlayer,
        PlaylistToggleQueued, PlaylistStopAfter, PlaylistSaveSelection, PlaylistSearchArtist,
        PlaylistSearchAlbum, PlaylistReloadInfo, PlaylistResetPlayCount, PlaylistRemoveRating,
        PlaylistReveal,    PlaylistTrash,
    };
    return table;
}

CommandAction commandAction(CommandId id) {
    // Names, not the enum's identifiers un-camel-cased: a name is read by a
    // person in a .ui file and typed into a shortcut editor, and "open-files"
    // says more than "file-open". The three radio groups share a name each
    // and differ by target, which is how a GAction expresses "one of these".
    switch (id) {
        case FileOpen:              return {"open-files", nullptr};
        case FileOpenFolder:        return {"open-folder", nullptr};
        case FileOpenUrl:           return {"open-url", nullptr};
        case FileSavePlaylist:      return {"save-playlist", nullptr};
        case FilePreferences:       return {"preferences", nullptr};
        case FileQuit:              return {"quit", nullptr};

        case EditUndo:              return {"undo", nullptr};
        case EditRedo:              return {"redo", nullptr};
        case EditSelectAll:         return {"select-all", nullptr};
        case EditScrollToCurrent:   return {"select-playing", nullptr};
        case EditRemove:            return {"remove", nullptr};
        case EditRandomize:         return {"randomize", nullptr};

        case PlaybackPlayPause:     return {"play-pause", nullptr};
        case PlaybackStop:          return {"stop", nullptr};
        case PlaybackNext:          return {"next", nullptr};
        case PlaybackPrevious:      return {"previous", nullptr};
        case PlaybackEnqueue:       return {"enqueue", nullptr};

        case OrderRepeatNone:       return {"repeat", "none"};
        case OrderRepeatOne:        return {"repeat", "one"};
        case OrderRepeatAlbum:      return {"repeat", "album"};
        case OrderRepeatAll:        return {"repeat", "all"};
        case OrderShuffleOff:       return {"shuffle", "off"};
        case OrderShuffleAlbums:    return {"shuffle", "albums"};
        case OrderShuffleAll:       return {"shuffle", "all"};

        case ViewFileTree:          return {"file-tree", nullptr};
        case ViewFileTreeRoot:      return {"file-tree-root", nullptr};
        case ViewSpectrum:          return {"spectrum", nullptr};
        case ViewOscilloscope:      return {"oscilloscope", nullptr};
        case ViewWaveform:          return {"waveform", nullptr};
        case ViewEqualizer:         return {"equalizer", nullptr};
        case ViewSpeed:             return {"speed", nullptr};
        case ViewInfo:              return {"info", nullptr};
        case ViewLyrics:            return {"lyrics", nullptr};
        case ViewTimedLyrics:       return {"timed-lyrics", nullptr};
        case ViewFollowSelection:   return {"panels-follow", "selection"};
        case ViewFollowPlayback:    return {"panels-follow", "playback"};
        case ViewSc55Panel:         return {"sc55", nullptr};
        case ViewDockPanes:         return {"reset-layout", nullptr};
        case ViewMiniPlayer:        return {"mini-player", nullptr};

        case PlaylistToggleQueued:  return {"toggle-queued", nullptr};
        case PlaylistStopAfter:     return {"stop-after", nullptr};
        case PlaylistSaveSelection: return {"save-selection", nullptr};
        case PlaylistSearchArtist:  return {"search-artist", nullptr};
        case PlaylistSearchAlbum:   return {"search-album", nullptr};
        case PlaylistReloadInfo:    return {"reload-info", nullptr};
        case PlaylistResetPlayCount: return {"reset-play-count", nullptr};
        case PlaylistRemoveRating:  return {"remove-rating", nullptr};
        case PlaylistReveal:        return {"reveal", nullptr};
        case PlaylistTrash:         return {"trash", nullptr};

        case HelpAbout:             return {"about", nullptr};

        case FirstWidgetId:
            break;
    }
    return {"", nullptr};
}




}  // namespace xpcog::app
