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
//
// **Timed lyrics are followed.** When the words are LRC -- in the tag, in a
// `.lrc` beside the file, or from LRCLIB -- the pane draws them one line per
// stamp and, while the track on screen is the one playing, marks the line
// being sung and keeps it in view. Where they came from, in order: the tag
// when it is LRC, then the `.lrc` file, then the tag as plain text, then the
// service, whose timed copy is preferred over its plain one. Timed beats plain
// between the listener's own sources because a file that can be followed is
// the better copy of the same words; the service still comes last, as before.
// See core/include/xpcog/core/lyrics/Lrc.hpp for what LRC is taken to be.
//
// `lyricsSynced` off (View -> Timed Lyrics) shows the same words as plain
// text instead, stamps stripped, and puts the tag back ahead of the `.lrc`
// file: timing was the only reason the file outranked it.
//
// The text stays a text control -- selectable, copyable, wrapping -- with the
// sung line styled in place rather than a custom-drawn karaoke view. And the
// following stops while the reader has text selected: scrolling a selection
// out from under someone copying it is the pane working against them.

#pragma once

#include "xpcog/core/lyrics/Lrc.hpp"
#include "xpcog/core/lyrics/LyricsLookup.hpp"

#include <wx/colour.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

/// Timed lyrics as the pane draws them: one line of text per stamp, in time
/// order, blank where the file marks a break. Free for the same reason as
/// normaliseLyrics(): the line numbering is what the highlight relies on, and
/// it can be pinned without a display.
[[nodiscard]] std::string syncedDisplayText(const SyncedLyrics& lyrics);

/// `colour` if it reads against `background`, otherwise `colour` moved towards
/// `text` just far enough that it does.
///
/// "Reads" is WCAG's 4.5:1, the ratio for body text. The sung line is drawn in
/// the desktop's accent, which is a colour chosen for sliders and switches and
/// says nothing about text: a yellow accent on a light pane or a dark blue one
/// on a dark pane is barely there. `text` is the pane's own foreground, the one
/// colour certain to contrast, so moving towards it always gets there -- and
/// stops as soon as it does, keeping as much of the accent as legibility allows.
[[nodiscard]] wxColour readableOn(const wxColour& colour, const wxColour& background,
                                  const wxColour& text);

class LyricsPanel : public wxPanel {
public:
    /// `position` reports how far into the playing track the listener has got,
    /// in seconds; it is asked only while a timed file is being followed.
    LyricsPanel(wxWindow* parent, std::function<double()> position);
    ~LyricsPanel() override;

    /// Draws `entry`'s lyrics, or the empty state when it is null or carries
    /// none. Null is ordinary -- nothing selected, nothing playing -- not an
    /// error. With a lookup set, an entry that carries none is asked about,
    /// and the pane says it is waiting until the answer lands. `playing` says
    /// whether `entry` is the track being played, which is what decides
    /// whether timed lyrics are followed or only shown.
    void showEntry(const PlaylistEntry* entry, bool playing);

    /// Where to ask for lyrics the file does not have, or null to stop
    /// asking. Borrowed; must outlive the panel or be cleared first. Called
    /// again with the same pointer when its server changes, which is a reason
    /// to redraw: the answer on screen may have come from the old one.
    void setLookup(LyricsLookup* lookup);

    /// Whether timed lyrics are shown timed, or as plain text. Takes effect on
    /// the next showEntry(), which the caller makes.
    void setTimed(bool timed) { timed_ = timed; }

private:
    /// What the pane asks about `entry`, or nullopt when the entry is not the
    /// kind of thing the service could know -- no artist, no title.
    [[nodiscard]] static std::optional<LyricsQuery> queryFor(const PlaylistEntry& entry);

    /// Draws `answer` for the track whose query key is `key`, if that is still
    /// the track on screen.
    void showAnswer(const std::string& key, const LyricsLookup::Answer& answer);

    /// Puts `body` on screen under the current heading, with `source` under
    /// it when not empty, and updates the redraw guard. `synced` is what the
    /// body was drawn from when the words are timed.
    void present(const wxString& body, const wxString& source,
                 std::optional<SyncedLyrics> synced);

    /// Starts or stops following, as `playing_` and the words on screen say.
    void updateFollowing();

    /// Marks the line being sung, if it has changed since the last tick.
    void tick();

    /// Styles line `index` of the timed text as sung or not.
    void styleLine(std::size_t index, bool sung);

    wxStaticText* heading_ = nullptr;
    wxTextCtrl*   text_    = nullptr;
    /// "From LRCLIB", or the `.lrc` file's name, shown only under words that
    /// did not come from the tag. The pane is the file's by default and a
    /// reader is entitled to know when it is not.
    wxStaticText* source_ = nullptr;

    std::function<double()> position_;
    wxTimer                 timer_;

    /// The timed words on screen, and where each line starts in the control.
    /// Empty when the words on screen are plain.
    std::optional<SyncedLyrics> synced_;
    std::vector<long>           lineStarts_;
    std::vector<long>           lineLengths_;
    /// The line currently marked as sung, or npos.
    std::size_t sung_ = SyncedLyrics::npos;

    /// The track on screen is the one playing.
    bool playing_ = false;

    /// `lyricsSynced`, as last handed over.
    bool timed_ = true;

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
