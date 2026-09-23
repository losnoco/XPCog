// Timed lyrics: the LRC format, and the `.lrc` file beside a track.
//
// LRC is a text file with a timestamp in front of each line --
// `[01:23.45]words` -- and a few `[tag:value]` headers. There is no standard,
// only the format as tools write it, and what is accepted here is what real
// files carry:
//
// - `[mm:ss]`, `[mm:ss.x]`, `[mm:ss.xx]`, `[mm:ss.xxx]`, and `[mm:ss:xx]`,
//   which some Windows taggers write with a colon where the dot goes. Minutes
//   are not limited to two digits.
// - Several stamps in front of one line, `[00:12.00][01:30.00]chorus`, which
//   is how a repeated line is written once. It becomes one line per stamp.
// - `[offset:+500]`, in milliseconds. Positive makes the words appear sooner,
//   which is what the format's authors meant and what every player does.
// - Word-level stamps from "enhanced" LRC, `<00:12.34>`, dropped: the line
//   is what is followed here, not the syllable.
// - A stamp with nothing after it, kept as an empty line. It is how a file
//   marks an instrumental break, and following the song means going blank
//   there rather than sitting on the last line sung.
//
// Every other header -- `[ar:]`, `[ti:]`, `[by:]`, `[length:]` -- is read past.
//
// **Where LRC turns up.** LRCLIB answers with it; `lrcget` and most lyrics
// plugins write it as `<track>.lrc` beside the audio; and taggers such as beets'
// lrclib plugin and MusicBee write it straight into the ordinary lyrics tag,
// so a FLAC's `LYRICS` or an MP3's USLT frame may be LRC rather than plain
// text. Which is why parseLrc() decides whether text *is* LRC rather than
// being told: the tag does not say.
//
// ID3's SYLT frame, the binary synchronised-lyrics frame, is not read. Almost
// nothing writes it, and nothing here maps it yet.

#pragma once

#include "xpcog/core/Url.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

struct LrcLine {
    /// Seconds into the track, the file's offset already applied. Never
    /// negative: an offset that pushes a line before the start clamps it there.
    double time = 0.0;
    /// The words, without stamps and without surrounding whitespace. Empty for
    /// a break.
    std::string text;
};

struct SyncedLyrics {
    /// In time order. Lines sharing a time keep the order the file had them.
    std::vector<LrcLine> lines;

    [[nodiscard]] bool empty() const noexcept { return lines.empty(); }

    /// The index of the line being sung at `seconds`: the last one whose time
    /// has been reached. npos before the first line, which is the silence (or
    /// the intro) before anyone sings.
    [[nodiscard]] std::size_t lineAt(double seconds) const;

    /// The words alone, one line per line, for showing when the song is not
    /// the one playing. Leading and trailing breaks are left out; inner ones
    /// become blank lines.
    [[nodiscard]] std::string plainText() const;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

/// `text` as timed lyrics, or nullopt when it is not LRC.
///
/// Text is LRC when at least one line carries a timestamp and timed lines
/// outnumber untimed ones -- blank lines and `[tag:value]` headers not counted
/// either way. That last part is what keeps plain lyrics with a `[Chorus]`
/// marker, or a single stray stamp in the middle, from being taken for a file
/// that can be followed. Untimed lines in a file that *is* LRC are dropped;
/// there is no moment to show them at.
[[nodiscard]] std::optional<SyncedLyrics> parseLrc(std::string_view text);

/// The `.lrc` file beside `url`'s file -- same folder, same name, `.lrc` for
/// the extension -- as UTF-8 text, or nullopt when there is none.
///
/// Only for a local file that is one track. A URL with a fragment is a track
/// inside something else -- a cue sheet's span, a subsong -- and the file
/// beside it would be timed against the whole file rather than the track.
/// A byte-order mark is dropped, and a file that is not UTF-8 is read as
/// Latin-1, which is what the older tools that wrote LRC produced. A file
/// larger than a lyrics file could plausibly be is refused rather than read.
[[nodiscard]] std::optional<std::string> readSidecarLrc(const Url& url);

}  // namespace xpcog
