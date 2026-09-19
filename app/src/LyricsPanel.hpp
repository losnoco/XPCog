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
//
// **When the file carries nothing, the pane can ask LRCLIB** -- new work, not
// port work; Cog's window is blank for an untagged file and that is the end of
// it. The file always wins: a tag is the listener's own, and a service is asked
// only about a track that has none. The asking is LyricsLookup's (core), which
// runs the request on a worker, remembers every answer in the library, and
// hands the result back on this thread; what the pane adds is the guard that
// an answer arriving for a track the listener has since moved off is dropped
// rather than drawn under the wrong heading. Off unless `enableLrclib` is on,
// which MainFrame expresses by handing over a lookup or a null.

#pragma once

#include "xpcog/core/lyrics/LyricsLookup.hpp"

#include <wx/panel.h>

#include <memory>
#include <optional>
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
    /// error. With a lookup set, an entry that carries none is asked about,
    /// and the pane says it is waiting until the answer lands.
    void showEntry(const PlaylistEntry* entry);

    /// Where to ask for lyrics the file does not have, or null to stop
    /// asking. Borrowed; must outlive the panel or be cleared first. Called
    /// again with the same pointer when its server changes, which is a reason
    /// to redraw: the answer on screen may have come from the old one.
    void setLookup(LyricsLookup* lookup);

private:
    /// What the pane asks about `entry`, or nullopt when the entry is not the
    /// kind of thing the service could know -- no artist, no title.
    [[nodiscard]] static std::optional<LyricsQuery> queryFor(const PlaylistEntry& entry);

    /// Draws `answer` for the track whose query key is `key`, if that is still
    /// the track on screen.
    void showAnswer(const std::string& key, const LyricsLookup::Answer& answer);

    /// Puts `body` on screen under the current heading, with the source line
    /// shown or not, and updates the redraw guard.
    void present(const wxString& body, bool fromService);

    wxStaticText* heading_ = nullptr;
    wxTextCtrl*   text_    = nullptr;
    /// "From LRCLIB", shown only under words that came from there. The pane is
    /// the file's by default and a reader is entitled to know when it is not.
    wxStaticText* source_ = nullptr;

    LyricsLookup* lookup_ = nullptr;

    /// The heading currently drawn, kept as a std::string beside the key
    /// because an answer arriving later redraws the body under it.
    std::string shownHeading_;

    /// The query key of the track on screen when it was asked about, or empty
    /// when the track on screen was not. An answer is drawn only if its key
    /// still matches: the lookup answers in order and the listener does not
    /// select in order.
    std::string awaiting_;

    /// Handed to every handler as a weak pointer, so an answer dispatched
    /// after the panel is gone finds nobody home.
    std::shared_ptr<int> alive_;

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
