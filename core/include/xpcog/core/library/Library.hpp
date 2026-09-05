// Persistence, replacing Core Data. Port of Cog's DataModel.xcdatamodeld plus
// the parts of PlaylistController.m that read and write it.
//
// Four Core Data entities become four tables. Three deliberate departures:
//
//  * **No metadataBlob.** Cog stores an NSKeyedArchiver transformable and then
//    materialises `spam`/`indexedSpam` derived properties purely to build a
//    search haystack, so filtering means deserialising every blob on every
//    keystroke. A child table of (entry, key, ordinal, value) round-trips
//    MetadataMap losslessly and makes search an indexable SQL query.
//  * **No sandbox.** `urlBookmark` and the whole SandboxToken entity exist for
//    the Mac App Store sandbox and are excised, not translated.
//  * **No soft deletes.** `deLeted` and `removed` exist because Core Data defers
//    deletion; SQLite just deletes the row.
//
// The database path is injected. Core does not know where an application
// support directory lives on any platform, and should not.

#pragma once

#include "xpcog/core/library/Playlist.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xpcog {

/// Cog's PlayCount entity. The natural key is (artist, album, title) -- the same
/// three-predicate fetch PlaylistEntry.m:687 performs, but indexed, so it is one
/// lookup rather than one per entry.
struct PlayCountRecord {
    std::string  artist;
    std::string  album;
    std::string  title;
    std::string  filename;
    std::int64_t count = 0;
    /// Unix seconds; 0 when never set.
    std::int64_t firstSeen  = 0;
    std::int64_t lastPlayed = 0;
    float        rating     = 0.0F;
};

class Library {
public:
    Library();
    ~Library();

    Library(const Library&)            = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&&) noexcept;
    Library& operator=(Library&&) noexcept;

    /// Opens or creates the database and applies any pending migrations.
    /// Pass ":memory:" for a throwaway database.
    [[nodiscard]] bool open(const std::filesystem::path& path);
    void               close();
    [[nodiscard]] bool isOpen() const;

    /// Empty when the last call succeeded.
    [[nodiscard]] std::string lastError() const;

    /// The schema version actually on disk.
    [[nodiscard]] int schemaVersion() const;

    // --- playlist -------------------------------------------------------

    /// Replaces the stored playlist with `playlist`, in one transaction.
    ///
    /// A whole-playlist rewrite rather than a diff: it is always consistent,
    /// and at 50k entries it is still one transaction of prepared-statement
    /// inserts. Incremental writes belong with the change notifications in the
    /// app layer, where there is something to be incremental about.
    [[nodiscard]] bool savePlaylist(const Playlist& playlist);

    /// Loads the stored playlist, replacing whatever `playlist` holds. Restores
    /// the queue, the shuffle order and the playing entry.
    [[nodiscard]] bool loadPlaylist(Playlist& playlist);

    /// Writes one entry's columns and tags. Used by the scanner as metadata
    /// arrives, so a long scan is not one transaction that can lose everything.
    [[nodiscard]] bool saveEntry(const PlaylistEntry& entry);

    // --- artwork --------------------------------------------------------

    /// Stores artwork content-addressed and returns its SHA-256 hex hash, which
    /// is what `PlaylistEntry::metadata["albumart"]` is replaced by on disk.
    /// Storing the same image twice stores it once.
    [[nodiscard]] std::string storeArtwork(std::span<const std::byte> data);

    [[nodiscard]] std::vector<std::byte> artwork(std::string_view hash) const;

    /// The same image, shared rather than copied out.
    ///
    /// One cover is asked for by the info panel, the now-playing display and the
    /// operating system's media integration, and each used to get its own copy
    /// read back out of the file -- three reads and three allocations of an
    /// image that may be fifteen megabytes. Callers holding the returned handle
    /// hold the same bytes.
    ///
    /// Cached weakly, plus a strong reference to the most recent one. Weak
    /// alone would miss the case this exists for, where the three callers ask in
    /// turn rather than at once; strong alone would accumulate every cover ever
    /// looked at. Returns an empty handle for an unknown hash.
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> sharedArtwork(
        std::string_view hash) const;

    /// Moves the image a metadata reader left in `entry.metadata["albumart"]`
    /// into the artwork table and replaces it with a hash. Returns false when
    /// there was nothing to move.
    ///
    /// Kept out of the Scanner because the Scanner has no database, and out of
    /// storeArtwork() because a caller may well have artwork with no entry.
    bool adoptArtwork(PlaylistEntry& entry);

    /// Deletes artwork no playlist entry references. Cog does this at startup
    /// (AppController.m:589).
    [[nodiscard]] std::int64_t pruneArtwork();

    // --- play counts ----------------------------------------------------

    [[nodiscard]] std::optional<PlayCountRecord> playCount(std::string_view artist,
                                                           std::string_view album,
                                                           std::string_view title) const;

    /// Records that the track exists without counting a play, and returns its
    /// row as it now stands. A track with no row gets one, tallying whatever
    /// count the entry already carries -- zero for an ordinary add, an imported
    /// number after a Cog import -- and `whenUnixSeconds` as its first-seen
    /// date. A track that already has a row keeps its date, and keeps the
    /// larger of the two tallies: an entry arriving with a count the row has
    /// never heard of is an import, and dropping it would leave the count on
    /// screen contradicting the one the next play increments.
    ///
    /// This is what makes `first_seen` mean what it says. Without it the row is
    /// created by the first recordPlay(), so a date that should read "when this
    /// entered the library" reads "sixty seconds into the first listen" -- and
    /// every track added and never played has no date at all.
    ///
    /// Returns nullopt only on failure; see lastError().
    [[nodiscard]] std::optional<PlayCountRecord> noteFirstSeen(
        const PlaylistEntry& entry, std::int64_t whenUnixSeconds);

    /// A tally another player kept for this track, ready to be merged in.
    /// Zero means "the source did not record this", for every field: Cog's
    /// dates and rating are all optional, and a zero one must not overwrite
    /// something this library already knows.
    struct PlayCountImport {
        std::int64_t count      = 0;
        std::int64_t firstSeen  = 0;  ///< Unix seconds.
        std::int64_t lastPlayed = 0;  ///< Unix seconds.
        double       rating     = 0.0;
    };

    /// Merges an imported tally into this track's row, creating it if needed.
    ///
    /// **The counts are merged by taking the larger, not by adding.** Cog's
    /// tally and XPCog's are two records of the same listening rather than two
    /// disjoint halves of it, so adding them would double a history that was
    /// only ever lived once -- and importing the same store twice, which is a
    /// thing a listener does when the first attempt matched badly, would double
    /// it again. Taking the larger is idempotent and never loses a count.
    ///
    /// The dates merge the way they read: the earliest first-seen and the
    /// latest last-played win, because that is what those words mean across two
    /// libraries. A rating already set here outranks the import -- it is the
    /// listener's own judgement, entered in this player.
    [[nodiscard]] bool importPlayCount(const PlaylistEntry&   entry,
                                       const PlayCountImport& imported);

    /// Increments the count for this entry, creating the row if needed.
    /// `whenUnixSeconds` is passed in rather than read from the clock, so the
    /// behaviour is testable and core stays free of a time source.
    [[nodiscard]] bool recordPlay(const PlaylistEntry& entry,
                                  std::int64_t         whenUnixSeconds);

    /// Sets this entry's tally back to zero, leaving the dates alone. Does
    /// nothing, successfully, when the track has no row -- see the definition.
    /// Cog's -resetPlayCountForTrack:.
    [[nodiscard]] bool resetPlayCount(const PlaylistEntry& entry);

    [[nodiscard]] bool setRating(const PlaylistEntry& entry, float rating);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog
