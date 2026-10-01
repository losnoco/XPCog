// What the Lyrics pane shows, decided without a toolkit.
//
// The pane draws three things -- a heading, a body and whether the body came
// from the service -- and deciding them is not drawing: which of the file's
// tags to show, what to say when it has none, whether to ask LRCLIB and what
// to say while waiting, and which answer still belongs to the track on screen
// by the time it arrives. That was the wx player's LyricsPanel.cpp's, and it moved
// here so both panes decide it the same way and the wording lives once.
//
// The one rule worth stating: **the service is asked only when the file has
// nothing**, and only about a track it could know. A tag is the listener's
// own and is never second-guessed.
//
// **Timed lyrics.** When the words are LRC -- in the tag, in a `.lrc` beside
// the file, or from LRCLIB -- the text is drawn one line per stamp and handed
// over with the timed lines, so a pane can mark the line being sung while the
// track plays. Where the words come from, in order: the tag when it is LRC,
// then the `.lrc` file, then the tag as plain text, then the service, whose
// timed copy is preferred over its plain one. Timed beats plain between the
// listener's own sources because a file that can be followed is the better
// copy of the same words; the service still comes last. With `lyricsSynced`
// off the same words are shown as plain text, stamps stripped, and the tag is
// put back ahead of the `.lrc` file: timing was the only reason the file
// outranked it. See core/include/xpcog/core/lyrics/Lrc.hpp for what LRC is
// taken to be.

#pragma once

#include "xpcog/core/lyrics/Lrc.hpp"
#include "xpcog/core/lyrics/LyricsLookup.hpp"

#include <cstdint>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace xpcog {
struct PlaylistEntry;
}

namespace xpcog::app {

/// Line endings and padding a tagger left behind, tidied: CRLF and lone CR
/// become LF, and blank lines at either end go.
[[nodiscard]] std::string normaliseLyrics(std::string text);

/// Timed lyrics as the pane draws them: one line of text per stamp, in time
/// order, blank where the file marks a break. Line N of this text is line N of
/// `lyrics`, which is what the highlight relies on.
[[nodiscard]] std::string syncedDisplayText(const SyncedLyrics& lyrics);

/// A colour, eight bits a channel, for the arithmetic below.
struct Rgb {
    std::uint8_t red   = 0;
    std::uint8_t green = 0;
    std::uint8_t blue  = 0;

    friend bool operator==(const Rgb&, const Rgb&) = default;
};

/// `colour` if it reads against `background`, otherwise `colour` moved towards
/// `text` just far enough that it does.
///
/// "Reads" is WCAG's 4.5:1, the ratio for body text. The sung line is drawn in
/// the desktop's accent, which is a colour chosen for sliders and switches and
/// says nothing about text: a yellow accent on a light pane or a dark blue one
/// on a dark pane is barely there. `text` is the pane's own foreground, the one
/// colour certain to contrast, so moving towards it always gets there -- and
/// stops as soon as it does, keeping as much of the accent as legibility allows.
[[nodiscard]] Rgb readableOn(Rgb colour, Rgb background, Rgb text);

/// What the pane draws.
struct LyricsText {
    std::string heading;  ///< "Artist -- Title", or the title alone.
    std::string body;     ///< The words, or the sentence saying why not.
    /// Where the words came from when not the tag -- "From LRCLIB", or the
    /// `.lrc` file -- translated, and empty for the tag's own. The pane is the
    /// file's by default and a reader is entitled to know when it is not.
    std::string source;
    /// The timed lines `body` was drawn from, when the words are followed;
    /// line N of `body` is line N of these.
    std::optional<SyncedLyrics> synced;
    /// The words carry timing, whether or not it is being followed: what a
    /// pane offers its timed toggle for, since turning timing off leaves
    /// `synced` empty.
    bool timeable = false;
};

/// Decides the text for a track, and asks the lookup when it should.
class LyricsPresenter {
public:
    LyricsPresenter();
    ~LyricsPresenter();

    LyricsPresenter(const LyricsPresenter&)            = delete;
    LyricsPresenter& operator=(const LyricsPresenter&) = delete;

    /// The lookup to ask, or null for a pane that shows the file's lyrics and
    /// nothing else. Whatever was being waited for is forgotten; the redraw
    /// the caller does next asks again.
    void setLookup(LyricsLookup* lookup);

    /// `lyricsSynced`: timed words shown timed, or as plain text. Takes effect
    /// on the next show(), which the caller makes.
    void setTimed(bool timed) { timed_ = timed; }

    /// The text for `entry`, now. When the file has none and the service is
    /// worth asking, the answer arrives later through `answered`, on the
    /// dispatcher's thread, unless another track has been shown by then. A
    /// null entry is the empty state.
    ///
    /// Answers nullopt when the text is exactly what the last call produced,
    /// so a pane redrawn on every selection change does not scroll back to the
    /// top for nothing -- keyed on heading and body both, because the same
    /// words under a different heading is a different track, and on whether
    /// the words are timed, because a timed file with no breaks and no
    /// repeats reads the same either way.
    [[nodiscard]] std::optional<LyricsText> show(const PlaylistEntry*                    entry,
                                                 std::function<void(const LyricsText&)> answered);

    /// Forgets what was last shown, so the next show() is drawn whatever it is.
    void invalidate() { shownKey_.clear(); }

private:
    [[nodiscard]] static std::optional<LyricsQuery> queryFor(const PlaylistEntry& entry);
    [[nodiscard]] static std::string keyOf(const LyricsText& text);
    [[nodiscard]] LyricsText fromFile(const PlaylistEntry& entry) const;
    [[nodiscard]] LyricsText bodyOf(std::string heading, const LyricsLookup::Answer& answer) const;

    LyricsLookup* lookup_ = nullptr;
    bool          timed_  = true;

    /// The key an arriving answer must match to be drawn: the query's, while a
    /// track without lyrics is being asked about, and empty otherwise.
    std::string awaiting_;
    std::string shownKey_;

    /// So an answer dispatched after this object has gone finds nothing.
    std::shared_ptr<int> alive_;
};

}  // namespace xpcog::app
