// Reading and writing playlist files. Port of Cog's PlaylistLoader.m save
// methods (lines 118-249) and Playlist/XmlContainer.m.
//
// Text in, text out: core parses and formats, and the caller does the file I/O.
// That keeps the formats testable without a filesystem, and keeps decisions
// about where a playlist may be written where they belong -- in the app.
//
// M3U and PLS are also registered as decoder-side containers, because opening
// one is a playback action. This is the other half: writing them, and the
// metadata-carrying formats that a URL list cannot express.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {

enum class PlaylistFormat {
    M3u,   ///< one path per line
    Pls,   ///< INI-shaped, FileN=path
    Xspf,  ///< XSPF 1.0. Cog cannot read or write this; most other players can.
    /// Cog's own format: an Apple XML property list carrying full metadata,
    /// embedded artwork and the play queue.
    CogXml,
};

/// Format for a file name, by extension. nullopt when it is not a playlist.
[[nodiscard]] std::optional<PlaylistFormat> playlistFormatForExtension(
    std::string_view extension);

/// A playlist file's contents, before ids are assigned.
struct PlaylistFileContents {
    std::vector<PlaylistEntry> entries;
    /// Indices into `entries`, in queue order.
    std::vector<std::size_t> queue;
    /// hash -> image bytes, referenced by `PlaylistEntry::artHash`. The caller
    /// stores these in the Library; carrying them separately is what stops one
    /// album's cover being repeated once per track.
    std::vector<std::pair<std::string, std::vector<std::byte>>> artwork;
};

/// Parses `text`. `source` is the playlist's own URL, used to resolve relative
/// paths. Returns nullopt only when the format is unrecognisable as such.
[[nodiscard]] std::optional<PlaylistFileContents> readPlaylist(
    PlaylistFormat format, std::string_view text, const Url& source);

/// Serialises `entries`. `destination` is where the file will be written, which
/// is what relative paths are relative to -- Cog does the same
/// (PlaylistLoader.m:98), and it is what makes a playlist survive moving a
/// music folder wholesale.
///
/// `artworkFor` supplies image bytes for an entry's `artHash` when the format
/// can embed them; pass nothing to write without artwork.
using ArtworkLookup = std::vector<std::byte> (*)(std::string_view hash, void* context);

[[nodiscard]] std::string writePlaylist(PlaylistFormat                    format,
                                        const std::vector<PlaylistEntry>& entries,
                                        const std::vector<std::size_t>&   queue,
                                        const Url&                        destination,
                                        ArtworkLookup artworkFor = nullptr,
                                        void*         context    = nullptr);

/// The path stored for `entry` in a playlist written to `destination`:
/// relative when both are local and share a directory prefix, absolute
/// otherwise. Exposed because every format needs it and it is worth testing on
/// its own.
[[nodiscard]] std::string relativePathFor(const Url& entry, const Url& destination);

}  // namespace xpcog
