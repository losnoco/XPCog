// One row of a playlist. Replaces Cog's PlaylistEntry (Playlist/PlaylistEntry.m,
// a Core Data NSManagedObject with 60-odd @dynamic properties).
//
// Two deliberate simplifications:
//
//  * Cog's -setMetadata: is one long if-else chain over "bitrate", "samplerate",
//    "totalframes" and friends, because decoders hand back a single dictionary
//    holding tags and stream properties together. XPCog already separates those
//    -- TrackProperties is a struct -- so applyMetadata() only handles tags.
//  * The derived Cocoa properties that exist purely to feed bindings (spam,
//    indexedSpam, statusMessage, lengthText, yearText, ...) are formatting, and
//    formatting belongs in the app layer. Only title() and display() survive
//    here, because playback ordering and scrobbling need them.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/TrackProperties.hpp"
#include "xpcog/core/Url.hpp"

#include <cstdint>
#include <string>

namespace xpcog {

/// Stable for the lifetime of a Playlist, and persisted as the library row id.
/// Cog identifies entries by their index in the arranged array, which is why
/// deleting a row while it plays needs the `deLeted`/`nextEntryAfterDeleted`
/// dance in PlaylistController; an id that does not move removes the problem.
using TrackId = std::uint64_t;

inline constexpr TrackId kInvalidTrackId = 0;

struct PlaylistEntry {
    TrackId id = kInvalidTrackId;
    Url     url;

    // --- promoted tags --------------------------------------------------
    // These are columns rather than map entries because they are sorted on,
    // grouped by and displayed for every row; going through MetadataMap for
    // each would turn a sort into a string search per comparison.
    std::string  album;
    std::string  albumArtist;
    std::string  artist;
    std::string  rawTitle;
    std::string  genre;
    std::string  composer;
    std::string  date;
    std::string  comment;
    std::string  unsyncedLyrics;
    std::int32_t track = 0;
    std::int32_t disc  = 0;
    std::int32_t year  = 0;

    /// Everything not promoted above, verbatim from the decoder.
    MetadataMap metadata;

    /// SHA-256 of the album art, or empty. The image itself is content-addressed
    /// in the library rather than carried per entry, so twelve tracks of one
    /// album hold one copy between them -- as in Cog (PlaylistEntry.m:447).
    std::string artHash;

    TrackProperties properties;

    // --- state ----------------------------------------------------------
    bool        metadataLoaded = false;
    bool        error          = false;
    std::string errorMessage;

    std::int64_t playCount       = 0;
    double       currentPosition = 0.0;  ///< seconds; only meaningful while current
    bool         stopAfter       = false;

    /// Position in the shuffle order, or -1 when not shuffled yet.
    std::int64_t shuffleIndex = -1;
    /// Position in the play queue, or -1 when not queued. Cog persists this and
    /// derives `queued` from it, so there is no separate flag.
    std::int32_t queuePosition = -1;

    [[nodiscard]] bool queued() const noexcept { return queuePosition >= 0; }

    /// The tag title, falling back to the file name. Cog's -title.
    [[nodiscard]] std::string title() const;

    /// "artist - title", or just the title when there is no artist. Cog's -display.
    [[nodiscard]] std::string display() const;

    /// Last path component, with the fragment appended when there is one, so cue
    /// tracks of the same file stay distinguishable. Cog's -filenameFragment.
    [[nodiscard]] std::string filename() const;

    [[nodiscard]] double duration() const noexcept { return properties.duration(); }

    /// Promotes the well-known tag names into columns and keeps the rest in
    /// `metadata`. Port of Cog's -setMetadata:, minus the property keys.
    void applyMetadata(const MetadataMap& tags);
};

}  // namespace xpcog
