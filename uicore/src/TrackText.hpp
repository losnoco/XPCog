// One track's fields, as text.
//
// Cog's -trackText, -lengthInfo, -gainInfo and -playCountInfo, which is where
// the wording and the rounding come from. None of it names a toolkit and all of
// it is wanted by anything that shows a track's details, so it lives here rather
// than in the panel that happened to need it first: app/src/InfoPanel.cpp
// renders these into an HTML page for wxHtmlWindow, and the GTK frontend puts
// the same strings in rows.
//
// tests/app/test_infopanel.cpp is what pins the wording down.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
class Library;
}

namespace xpcog::app::info {

/// The fields the info pane shows, in Cog's order. The three groups are where
/// Cog's own order already breaks: who made it, what it is, and what we know
/// about it.
enum Field {
    AlbumArtist,
    Artist,
    Composer,
    Album,
    Title,
    Track,
    Length,
    Date,
    Genre,
    Filename,

    SampleRate,
    Channels,
    Bitrate,
    BitsPerSample,
    Codec,
    Encoding,

    Cuesheet,
    ReplayGain,
    PlayCount,
    Comment,

    FieldCount,
};

/// Cog's label for each field, as a msgid: marked rather than translated,
/// because the lookup happens where the pane is drawn. Several are also
/// playlist column headings -- Title, Artist, Album -- and share a msgid with
/// those on purpose: one word, one translation, whichever surface shows it.
[[nodiscard]] const std::array<const char*, FieldCount>& fieldLabels();

/// Every field's text for `entry`, indexed by Field, empty where the entry has
/// nothing to say. `library` supplies the play-count dates and may be null.
///
/// What the wx panel's showEntry() did, taken out so both info panes fill
/// their rows from one place: the "yes"/"no" for a cue sheet, the whole path
/// rather than Cog's last component, the fallback from date to year.
[[nodiscard]] std::array<std::string, FieldCount> describe(const PlaylistEntry& entry,
                                                           const Library*       library);



/// Cog's -trackText: "03", or "1.03" when the disc is known, or empty.
[[nodiscard]] std::string trackText(std::int32_t track, std::int32_t disc);

/// Cog's -lengthInfo, which unlike the playlist column keeps the fraction --
/// this is the panel you open when you care whether a gapless rip is 4:07.000.
[[nodiscard]] std::string lengthText(double seconds);

/// Cog's -gainInfo: every gain value that is present, one per line. Empty when
/// the file carries none, which is most files.
[[nodiscard]] std::string replayGainText(const ReplayGainInfo& gain);

/// Cog's -playCountInfo, with the count on the first line. `firstSeen` and
/// `lastPlayed` are Unix seconds; 0 means never set.
[[nodiscard]] std::string playCountText(std::int64_t count, std::int64_t firstSeen,
                                        std::int64_t lastPlayed);

/// The pieces a value may be broken between: after the platform's path
/// separator, and after a space, which is a break wxHTML would take anyway.
///
/// Concatenating the result gives the input back exactly -- each piece keeps the
/// character that ended it -- so what is measured is what is drawn.
[[nodiscard]] std::vector<std::string> breakPieces(std::string_view text);

}  // namespace xpcog::app::info

namespace xpcog::app {

/// The transport's clock: `m:ss`, or `h:mm:ss` once past an hour.
///
/// **Truncated, not rounded**, which is the whole reason this is a function with
/// a comment rather than three copies of a one-liner. At 0.9 s the listener is
/// still inside the first second, and a clock reading 0:01 there is half a second
/// early for the whole track -- most visibly at a track's start, where the label
/// appears at 0:01 before a second has played and then sits there until the
/// playhead catches up, which reads as the clock starting late and freezing.
/// Cog truncates in both places it formats a time: `(long)value` in
/// PositionSlider.m, `(unsigned)[object doubleValue]` in SecondsFormatter.m.
[[nodiscard]] std::string formatClock(double seconds);

}  // namespace xpcog::app
