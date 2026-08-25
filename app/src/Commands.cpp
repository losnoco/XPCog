#include "Commands.hpp"

#include "Text.hpp"

#include <wx/menu.h>
#include <wx/translation.h>

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
            // Translated here rather than in the table: wxTRANSLATE only marks
            // a literal, so the catalogue is consulted at the moment the menu is
            // built -- which is what lets the same table be read for a tray menu
            // or a context menu without each of them remembering to translate.
            title   = trUtf8(item.menu);
        }
        if (current == nullptr) {
            continue;
        }
        if (item.separatorBefore) {
            current->AppendSeparator();
        }

        wxString label = trUtf8(item.label);
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
