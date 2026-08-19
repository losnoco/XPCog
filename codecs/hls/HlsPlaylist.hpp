// The m3u8 manifest: what it says, and how to resolve what it points at.
//
// Port of Cog Plugins/HLS/HLSPlaylistParser.m and the three model classes beside
// it (HLSPlaylist, HLSSegment, HLSVariant), which are plain property bags and
// collapse into structs here.
//
// Kept away from sockets and threads on purpose. A manifest parser is a pure
// function from text to a segment list, and every case worth checking -- an
// attribute list with a comma inside a quoted CODECS value, a key or map tag
// that applies forward to every following segment, a relative URI three
// directories up -- is reachable by handing it a string. The fetching that uses
// the result lives in HlsSegmentManager, where none of that can be tested.

#pragma once

#include "xpcog/core/Url.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::codecs {

enum class HlsPlaylistType : std::uint8_t { Unspecified, Event, Vod };

struct HlsSegment {
    Url          url;
    double       duration              = 0.0;
    std::int64_t sequenceNumber        = 0;
    std::int64_t discontinuitySequence = 0;
    bool         discontinuity         = false;

    /// EXT-X-KEY, applied forward from the tag that declared it. Nothing here
    /// decrypts; the decoder refuses a playlist whose segments are encrypted, as
    /// Cog's does. Parsed rather than ignored so that refusal can be specific
    /// instead of surfacing as a corrupt-stream failure four layers down.
    bool                   encrypted = false;
    std::string            encryptionMethod;
    Url                    encryptionKeyUrl;
    std::vector<std::byte> iv;

    /// EXT-X-MAP: the initialisation section a fragmented-MP4 rendition needs
    /// before its first segment. Recorded; see HlsSegmentManager for what is
    /// done with it.
    Url         mapUrl;
    std::string title;
};

struct HlsVariant {
    std::int64_t bandwidth        = 0;
    std::int64_t averageBandwidth = 0;
    std::string  codecs;
    std::string  resolution;
    Url          url;
};

struct HlsPlaylist {
    /// Where the manifest was fetched from. Relative URIs resolve against this,
    /// so it must be the URL actually read -- after redirects, not before.
    Url url;

    bool            isMaster              = false;
    /// True until an EXT-X-ENDLIST or PLAYLIST-TYPE:VOD says otherwise. An EVENT
    /// playlist stays live: it only ever grows, but it does grow.
    bool            isLive                = true;
    bool            hasEndList            = false;
    int             version               = 1;
    int             targetDuration        = 10;
    std::int64_t    mediaSequence         = 0;
    std::int64_t    discontinuitySequence = 0;
    HlsPlaylistType type                  = HlsPlaylistType::Unspecified;

    std::vector<HlsSegment> segments;
    std::vector<HlsVariant> variants;

    /// Summed segment durations, or 0 for a live playlist, where the number
    /// would be the length of the window rather than of anything playable.
    [[nodiscard]] double totalDuration() const;

    /// The rendition to play from a master playlist: the highest bandwidth on
    /// offer, matching Cog. Audio-only masters list one entry per bitrate and
    /// there is no adaptive switching here, so the best one is the only sensible
    /// fixed choice. Null when there is nothing usable.
    [[nodiscard]] const HlsVariant* bestVariant() const;
};

/// Resolves an HLS URI reference against the manifest it appeared in, per
/// RFC 3986 section 5. Absolute references pass through; `//host/x`, `/x`, `x`
/// and `../x` all resolve against `base`.
///
/// Written here rather than reusing codecs/common's resolveEntry(), which
/// resolves against a *filesystem* path and would turn `segment0.ts` beside an
/// https manifest into a file:// URL naming nothing.
[[nodiscard]] std::optional<Url> resolveHlsUri(std::string_view reference,
                                               const Url&       base);

/// Whether `text` is an HLS manifest rather than an ordinary M3U track list.
///
/// The discriminator is the tag RFC 8216 makes mandatory for each kind:
/// EXT-X-TARGETDURATION for a media playlist, EXT-X-STREAM-INF for a master.
/// Matching on `#EXTM3U` alone would claim every M3U in the world, and matching
/// on any `#EXT-X-` tag would claim a playlist carrying only, say, EXT-X-VERSION
/// -- which a media playlist without a target duration is not.
[[nodiscard]] bool looksLikeHlsManifest(std::string_view text);

/// Parses `text` as an m3u8 manifest. Returns nullopt when it is not one, when
/// it fails looksLikeHlsManifest(), or when it declares neither segments nor
/// variants.
[[nodiscard]] std::optional<HlsPlaylist> parseHlsPlaylist(std::string_view text,
                                                          const Url&       base);

/// RFC 8216 section 4.2 attribute lists: `A=1,B="x,y",C=0xFF`. Splits at
/// top-level commas only, so a quoted CODECS value survives, and strips the
/// quotes. Exposed for tests.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parseAttributeList(
    std::string_view attributes);

}  // namespace xpcog::codecs
