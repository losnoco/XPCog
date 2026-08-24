// The lyrics a file carries, for the track you are looking at.
//
// Port of Cog's LyricsWindowController (LyricsWindow/, 63 lines and a XIB): a
// read-only text view showing `unsyncedlyrics`, following the playlist selection
// when there is one and the playing track otherwise. That rule is Cog's, from the
// same observer InfoPanel copies, and it is the right one here for the same
// reason -- following the selection alone blanks the pane every time you click
// empty space, and following playback alone makes it useless for looking
// something up.
//
// **Nothing had to be read to build this.** The lyrics were already arriving:
// codecs/flac and codecs/common/VorbisComments normalise `unsynced lyrics` and
// `lyrics` to `unsyncedlyrics` exactly as Cog's decoders do, PlaylistEntry
// promotes that to a column, and Library stores and restores it. What was missing
// was somewhere to look. MP3s work too, which Cog's do not: Cog reads lyrics from
// its FLAC, Vorbis, Opus and FFmpeg decoders and its TagLib plugin has no lyrics
// handling at all, while codecs/taglib here goes through TagLib's PropertyMap,
// which maps ID3's USLT frame onto LYRICS.
//
// A dockable pane rather than Cog's floating window, which is the same choice
// InfoPanel made and for the same reason: Cog positions its lyrics window to the
// right of the main window by hand (`toggleWindow:` computes the frame), and a
// pane is that with less arithmetic and with the position remembered.
//
// Like InfoPanel, this holds no reference to the entry it drew. Playlist entries
// move when rows are removed or the playlist reloads; it copies the strings it
// needs on the way through.

#pragma once

#include <wx/panel.h>

#include <string>

class wxStaticText;
class wxTextCtrl;

namespace xpcog {
struct PlaylistEntry;
}

namespace xpcog::app {

/// Normalises what a tag actually contains into what a text control can show.
///
/// Free and separately declared because it is the part worth testing without a
/// display: taggers write lyrics with CRLF, with a lone CR, and with trailing
/// blank lines, and a stray `\r` reaching a GTK text control is a visible box
/// rather than a line break.
[[nodiscard]] std::string normaliseLyrics(std::string text);

class LyricsPanel : public wxPanel {
public:
    explicit LyricsPanel(wxWindow* parent);

    /// Draws `entry`'s lyrics, or the empty state when it is null or carries
    /// none. Null is ordinary -- nothing selected, nothing playing -- not an
    /// error.
    void showEntry(const PlaylistEntry* entry);

private:
    wxStaticText* heading_ = nullptr;
    wxTextCtrl*   text_    = nullptr;

    /// What is currently drawn, so that a refresh for the same track is a no-op.
    ///
    /// Not an optimisation. showEntry() is called on every selection change and
    /// every current-track change, and a wxTextCtrl scrolls back to the top when
    /// its value is replaced -- so without this, metadata arriving mid-scan
    /// yanks the reader back to the first line of a song they are halfway
    /// through.
    std::string shownKey_;
};

}  // namespace xpcog::app
