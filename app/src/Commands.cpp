#include "Commands.hpp"

#include <wx/menu.h>

#include <map>

namespace xpcog::app {
namespace {

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
        {"&File", FileOpen, "&Open Files...", "Ctrl+O"},
        {nullptr, FileOpenFolder, "Open &Folder...", "Ctrl+Shift+O"},
        {nullptr, FileOpenUrl, "Open &URL...", "Ctrl+L"},
        {nullptr, FileSavePlaylist, "&Save Playlist...", "Ctrl+S", ItemKind::Normal, true},
        {nullptr, FilePreferences, "&Preferences...", "Ctrl+,", ItemKind::Normal, true},
        {nullptr, FileQuit, "&Quit", "Ctrl+Q", ItemKind::Normal, true},

        {"&View", ViewFileTree, "&File Browser", "Ctrl+B", ItemKind::Check},
        // Not checkable: it opens a dialog rather than showing or hiding
        // anything. Directly under the browser it belongs to, because a root you
        // cannot find is a browser stuck wherever it was left.
        {nullptr, ViewFileTreeRoot, "Choose &Root Folder...", ""},
        // Cog's Info Inspector, on Cog's shortcut.
        {nullptr, ViewInfo, "&Info", "Ctrl+I", ItemKind::Check, true},
        // Cog's Lyrics window, on Cog's shortcut and directly under Info, which
        // is where Cog groups it too. Cog labels it "Show Lyrics"; the items in
        // this menu are checkable toggles that say what they show rather than
        // what they do, so it is "&Lyrics" here. Ctrl+L is Open URL, so the
        // shifted form is not a second choice -- it is Cog's own (Cmd+Shift+L).
        {nullptr, ViewLyrics, "&Lyrics", "Ctrl+Shift+L", ItemKind::Check},
        {nullptr, ViewSpectrum, "&Spectrum", "Ctrl+U", ItemKind::Check},
        // Cog keeps its equaliser in a window of its own rather than in
        // preferences, and so does this. Ctrl-E, which nothing else claims.
        {nullptr, ViewEqualizer, "&Equalizer", "Ctrl+E", ItemKind::Check},
        // The SC-55's front panel. No shortcut: it is worth having and it is not
        // worth a key -- one synthesiser of three, for one format. Present even
        // in a build without MIDI, where it simply never finds a pane to toggle;
        // a command that appears and disappears with a compile flag is worse
        // than one that is occasionally inert.
        {nullptr, ViewSc55Panel, "SC-55 &Panel", "", ItemKind::Check},
        {nullptr, ViewMiniPlayer, "&Mini Player", "Ctrl+M", ItemKind::Check, true},

        {"&Edit", EditUndo, "&Undo", "Ctrl+Z"},
        {nullptr, EditRedo, "&Redo", "Ctrl+Y"},
        {nullptr, EditSelectAll, "Select &All", "Ctrl+A", ItemKind::Normal, true},
        {nullptr, EditRemove, "&Remove from Playlist", "Del"},
        {nullptr, EditRandomize, "Randomi&ze Playlist", "", ItemKind::Normal, true},

        {"&Playback", PlaybackPlayPause, "&Play/Pause", ""},
        {nullptr, PlaybackStop, "&Stop", "Ctrl+."},
        {nullptr, PlaybackPrevious, "Pre&vious", "Ctrl+Left", ItemKind::Normal, true},
        {nullptr, PlaybackNext, "&Next", "Ctrl+Right"},
        {nullptr, PlaybackEnqueue, "Add to &Queue", "Q", ItemKind::Normal, true},

        // Two exclusive sets. Cog drives these from an NSPopUpButton plus four
        // NSValueTransformers; consecutive radio items are the same idea with
        // the transformers deleted.
        //
        // The separator between the Repeat four and the Shuffle three is
        // load-bearing and must not be removed: a separator breaks a radio run,
        // which is exactly what keeps these two groups from becoming one.
        {"&Order", OrderRepeatNone, "Repeat: &Off", "", ItemKind::Radio},
        {nullptr, OrderRepeatOne, "Repeat: &One", "", ItemKind::Radio},
        {nullptr, OrderRepeatAlbum, "Repeat: &Album", "", ItemKind::Radio},
        {nullptr, OrderRepeatAll, "Repeat: A&ll", "", ItemKind::Radio},
        {nullptr, OrderShuffleOff, "Shuffle: O&ff", "", ItemKind::Radio, true},
        {nullptr, OrderShuffleAlbums, "Shuffle: Al&bums", "", ItemKind::Radio},
        {nullptr, OrderShuffleAll, "Shuffle: A&ll Tracks", "", ItemKind::Radio},

        // No "About Qt" any more, and nothing replaces it: the toolkit's version
        // is a row in the About box's component list, which is where it belongs.
        {"&Help", HelpAbout, "&About XPCog", ""},
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

}  // namespace

const std::vector<MenuItem>& menuLayout() { return layout(); }

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
            title   = wxString::FromAscii(item.menu);
        }
        if (current == nullptr) {
            continue;
        }
        if (item.separatorBefore) {
            current->AppendSeparator();
        }

        wxString label = wxString::FromAscii(item.label);
        if (*item.accelerator != '\0') {
            label += "\t";
            label += wxString::FromAscii(item.accelerator);
        }
        current->Append(item.id, label, wxEmptyString, toWx(item.kind));
    }
    flush();

    return bar;
}

}  // namespace xpcog::app
