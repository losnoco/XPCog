#include "Commands.hpp"

#include "Text.hpp"

#include <wx/menu.h>
#include <wx/translation.h>

#include <map>

namespace xpcog::app {
namespace {

// Two labels that name something the platform names differently. One string
// serving all three would have to be generic enough to be wrong everywhere --
// nobody on macOS looks for a "file manager" -- so the literal is chosen at
// compile time. All three spellings still reach the catalogue: the extractor
// reads the source rather than the preprocessor's output, so a translator sees
// every branch whichever platform they are on.
#if defined(__WXOSX__)
constexpr const char* kRevealLabel = wxTRANSLATE("Show in &Finder");
#elif defined(__WXMSW__)
constexpr const char* kRevealLabel = wxTRANSLATE("Show in &Explorer");
#else
constexpr const char* kRevealLabel = wxTRANSLATE("Show in &File Manager");
#endif

#if defined(__WXMSW__)
constexpr const char* kTrashLabel = wxTRANSLATE("Move to the Recycle &Bin");
#else
constexpr const char* kTrashLabel = wxTRANSLATE("Move to &Trash");
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
        {wxTRANSLATE("&File"), FileOpen, wxTRANSLATE("&Open Files..."), "Ctrl+O"},
        {nullptr, FileOpenFolder, wxTRANSLATE("Open &Folder..."), "Ctrl+Shift+O"},
        {nullptr, FileOpenUrl, wxTRANSLATE("Open &URL..."), "Ctrl+L"},
        // No accelerator, deliberately: this is a thing somebody does once, when
        // they move over from Cog, and a shortcut for it would be occupying a
        // key for the rest of the installation's life.
        {nullptr, FileImportCog, wxTRANSLATE("&Import from Cog..."), "", ItemKind::Normal, true},
        {nullptr, FileSavePlaylist, wxTRANSLATE("&Save Playlist..."), "Ctrl+S", ItemKind::Normal, true},
        {nullptr, FilePreferences, wxTRANSLATE("&Preferences..."), "Ctrl+,", ItemKind::Normal, true},
        {nullptr, FileQuit, wxTRANSLATE("&Quit"), "Ctrl+Q", ItemKind::Normal, true},

        {wxTRANSLATE("&View"), ViewFileTree, wxTRANSLATE("&File Browser"), "Ctrl+B", ItemKind::Check},
        // Not checkable: it opens a dialog rather than showing or hiding
        // anything. Directly under the browser it belongs to, because a root you
        // cannot find is a browser stuck wherever it was left.
        {nullptr, ViewFileTreeRoot, wxTRANSLATE("Choose &Root Folder..."), ""},
        // Cog's Info Inspector, on Cog's shortcut.
        {nullptr, ViewInfo, wxTRANSLATE("&Info"), "Ctrl+I", ItemKind::Check, true},
        // Cog's Lyrics window, on Cog's shortcut and directly under Info, which
        // is where Cog groups it too. Cog labels it "Show Lyrics"; the items in
        // this menu are checkable toggles that say what they show rather than
        // what they do, so it is "&Lyrics" here. Ctrl+L is Open URL, so the
        // shifted form is not a second choice -- it is Cog's own (Cmd+Shift+L).
        {nullptr, ViewLyrics, wxTRANSLATE("&Lyrics"), "Ctrl+Shift+L", ItemKind::Check},
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
        {nullptr, ViewFollowSelection, wxTRANSLATE("Panels Follow &Selection"), "", ItemKind::Radio},
        {nullptr, ViewFollowPlayback, wxTRANSLATE("Panels Follow Play&back"), "", ItemKind::Radio},
        {nullptr, ViewSpectrum, wxTRANSLATE("&Spectrum"), "Ctrl+U", ItemKind::Check, true},
        // Cog keeps its equaliser in a window of its own rather than in
        // preferences, and so does this. Ctrl-E, which nothing else claims.
        {nullptr, ViewEqualizer, wxTRANSLATE("&Equalizer"), "Ctrl+E", ItemKind::Check},
        // The SC-55's front panel. No shortcut: it is worth having and it is not
        // worth a key -- one synthesiser of three, for one format. Present even
        // in a build without MIDI, where it simply never finds a pane to toggle;
        // a command that appears and disappears with a compile flag is worse
        // than one that is occasionally inert.
        {nullptr, ViewSc55Panel, wxTRANSLATE("SC-55 &Panel"), "", ItemKind::Check},
        {nullptr, ViewMiniPlayer, wxTRANSLATE("&Mini Player"), "Ctrl+M", ItemKind::Check, true},

        {wxTRANSLATE("&Edit"), EditUndo, wxTRANSLATE("&Undo"), "Ctrl+Z"},
        {nullptr, EditRedo, wxTRANSLATE("&Redo"), "Ctrl+Y"},
        {nullptr, EditSelectAll, wxTRANSLATE("Select &All"), "Ctrl+A", ItemKind::Normal, true},
        {nullptr, EditRemove, wxTRANSLATE("&Remove from Playlist"), "Del"},
        {nullptr, EditRandomize, wxTRANSLATE("Randomi&ze Playlist"), "", ItemKind::Normal, true},

        {wxTRANSLATE("&Playback"), PlaybackPlayPause, wxTRANSLATE("&Play/Pause"), ""},
        {nullptr, PlaybackStop, wxTRANSLATE("&Stop"), "Ctrl+."},
        {nullptr, PlaybackPrevious, wxTRANSLATE("Pre&vious"), "Ctrl+Left", ItemKind::Normal, true},
        {nullptr, PlaybackNext, wxTRANSLATE("&Next"), "Ctrl+Right"},
        {nullptr, PlaybackEnqueue, wxTRANSLATE("Add to &Queue"), "Q", ItemKind::Normal, true},

        // Two exclusive sets. Cog drives these from an NSPopUpButton plus four
        // NSValueTransformers; consecutive radio items are the same idea with
        // the transformers deleted.
        //
        // The separator between the Repeat four and the Shuffle three is
        // load-bearing and must not be removed: a separator breaks a radio run,
        // which is exactly what keeps these two groups from becoming one.
        {wxTRANSLATE("&Order"), OrderRepeatNone, wxTRANSLATE("Repeat: &Off"), "", ItemKind::Radio},
        {nullptr, OrderRepeatOne, wxTRANSLATE("Repeat: &One"), "", ItemKind::Radio},
        {nullptr, OrderRepeatAlbum, wxTRANSLATE("Repeat: &Album"), "", ItemKind::Radio},
        {nullptr, OrderRepeatAll, wxTRANSLATE("Repeat: A&ll"), "", ItemKind::Radio},
        {nullptr, OrderShuffleOff, wxTRANSLATE("Shuffle: O&ff"), "", ItemKind::Radio, true},
        {nullptr, OrderShuffleAlbums, wxTRANSLATE("Shuffle: Al&bums"), "", ItemKind::Radio},
        {nullptr, OrderShuffleAll, wxTRANSLATE("Shuffle: A&ll Tracks"), "", ItemKind::Radio},

        // No "About Qt" any more, and nothing replaces it: the toolkit's version
        // is a row in the About box's component list, which is where it belongs.
        {wxTRANSLATE("&Help"), HelpAbout, wxTRANSLATE("&About XPCog"), ""},
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
        {nullptr, PlaylistToggleQueued, wxTRANSLATE("Add to &Queue"), ""},
        {nullptr, PlaylistStopAfter, wxTRANSLATE("Stop after &Selection"), ""},
        {nullptr, PlaylistSaveSelection, wxTRANSLATE("Save Selection as &Playlist..."),
         "", ItemKind::Normal, true},
        {nullptr, PlaylistSearchArtist, wxTRANSLATE("Search for &Artist"), "",
         ItemKind::Normal, true},
        {nullptr, PlaylistSearchAlbum, wxTRANSLATE("Search for Al&bum"), ""},
        {nullptr, PlaylistReloadInfo, wxTRANSLATE("Re&load Info"), "", ItemKind::Normal,
         true},
        {nullptr, PlaylistResetPlayCount, wxTRANSLATE("Reset Play &Count"), ""},
        {nullptr, PlaylistRemoveRating, wxTRANSLATE("Remove Ra&ting"), ""},
        {nullptr, PlaylistReveal, kRevealLabel, ""},
        {nullptr, EditRemove, wxTRANSLATE("&Remove from Playlist"), "Del",
         ItemKind::Normal, true},
        {nullptr, PlaylistTrash, kTrashLabel, ""},
        {nullptr, ViewInfo, wxTRANSLATE("&Info"), "", ItemKind::Check, true},
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

[[nodiscard]] wxItemKind toWx(ItemKind kind) {
    switch (kind) {
        case ItemKind::Check: return wxITEM_CHECK;
        case ItemKind::Radio: return wxITEM_RADIO;
        case ItemKind::Normal: break;
    }
    return wxITEM_NORMAL;
}

/// One row, onto whichever menu is being built.
void appendItem(wxMenu& menu, const MenuItem& item) {
    if (item.separatorBefore) {
        menu.AppendSeparator();
    }

    wxString label = trUtf8(item.label);
    if (*item.accelerator != '\0') {
        label += "\t";
        label += wxString::FromAscii(item.accelerator);
    }
    menu.Append(item.id, label, wxEmptyString, toWx(item.kind));
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

wxMenuBar* buildMenuBar() {
    auto*    bar     = new wxMenuBar;
    wxMenu*  current = nullptr;
    wxString title;

    const auto flush = [&] {
        if (current != nullptr) {
            bar->Append(current, title);
        }
    };

    for (const MenuItem& item : menuLayout()) {
        if (item.menu != nullptr) {
            flush();
            current = new wxMenu;
            // Translated here rather than in the table: wxTRANSLATE only marks
            // a literal, so the catalogue is consulted at the moment the menu is
            // built -- which is what lets the same table be read for a tray menu
            // or a context menu without each of them remembering to translate.
            title   = trUtf8(item.menu);
        }
        if (current == nullptr) {
            continue;
        }
        appendItem(*current, item);
    }
    flush();

    return bar;
}

wxMenu* buildMenu(const std::vector<MenuItem>& items) {
    auto* menu = new wxMenu;
    for (const MenuItem& item : items) {
        appendItem(*menu, item);
    }
    return menu;
}

}  // namespace xpcog::app
