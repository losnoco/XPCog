#include "xpcog/core/library/CogImport.hpp"

#include "Sqlite.hpp"

#include <algorithm>
#include <utility>

namespace xpcog {
namespace {

/// Cog's own prune, from PlaylistLoader.m:1259 --
///
///     pe.deLeted || !pe.urlString || ![pe.urlString length]
///
/// Applied here rather than left to the caller because an entry Cog will not
/// show is not part of the playlist being imported, and a reader that returned
/// them would be describing the file rather than the library.
[[nodiscard]] bool pruned(bool deleted, const std::string& urlString) {
    return deleted || urlString.empty();
}

/// The columns, in the order the query selects them. Named so the reader below
/// is not a column-index puzzle -- the query and this enum have to be edited
/// together, and being adjacent is what makes that likely.
enum Column {
    kIndex = 0,
    kUrlString,
    kDeleted,
    kTotalFrames,
    kSampleRate,
    kBitrate,
    kChannels,
    kCodec,
    kTrackGain,
    kTrackPeak,
    kAlbumGain,
    kAlbumPeak,
    kCurrent,
    kCurrentPosition,
    kQueued,
    kQueuePosition,
    kShuffleIndex,
};

/// ZQUEUED and ZREMOVED are Core Data optional Booleans, which arrive as NULL
/// rather than 0 when they have never been set. Testing for truth rather than
/// for presence is the difference between "not queued" and "queued, unknown".
[[nodiscard]] bool truthy(const sql::Statement& statement, int column) {
    return !statement.isNull(column) && statement.columnInt(column) != 0;
}

}  // namespace

bool isCogFileReferenceUrl(std::string_view urlString) {
    // Both spellings Cog tests for, from Utils/CogURLNormalization.h: the
    // serialised URL, and the bare path a drag can hand over.
    return urlString.starts_with("file:///.file/id=") ||
           urlString.starts_with("/.file/id=");
}

CogPlayCounts::CogPlayCounts(std::vector<CogPlayCount> counts)
    : counts_(std::move(counts)) {}

const CogPlayCount* CogPlayCounts::find(std::string_view filename,
                                        std::string_view title,
                                        std::string_view artist,
                                        std::string_view album) const {
    // Linear, and that is not laziness: this is asked once per track during an
    // import of a few thousand, against a table of the same order, and the
    // alternative is a key that has to encode "this field does not constrain
    // the match" -- which is the whole subtlety, and would be hidden inside a
    // hash rather than stated.
    //
    // A field Cog left empty is skipped rather than required to be empty on the
    // track. It cannot disagree with anything, and requiring it to match would
    // discard the 78 rows in 84 that carry no artist or album.
    const auto agrees = [](std::string_view stored, std::string_view actual) {
        return stored.empty() || stored == actual;
    };

    for (const CogPlayCount& count : counts_) {
        if (agrees(count.filename, filename) && agrees(count.title, title) &&
            agrees(count.artist, artist) && agrees(count.album, album)) {
            return &count;
        }
    }
    return nullptr;
}

std::optional<CogLibrary> readCogLibrary(const std::filesystem::path& store) {
    // Read-only, and it has to be. This is another program's database, quite
    // possibly the live one -- opening it read-write would rewrite its header to
    // set a journal mode, and SQLITE_OPEN_CREATE would turn a wrong path into a
    // new empty file rather than into an error.
    sql::Database database;
    if (!database.openReadOnly(store)) {
        return std::nullopt;
    }

    CogLibrary library;

    // ZINDEX, not Z_PK. They disagree on a real store -- Z_PK is insertion
    // order -- and a reader that took the natural order would be right on a
    // freshly built playlist and wrong on every one that had been reordered,
    // which is the worst way for this to be wrong.
    sql::Statement entries{
        database,
        "select ZINDEX, ZURLSTRING, ZDELETED, ZTOTALFRAMES, ZSAMPLERATE, ZBITRATE, "
        "ZCHANNELS, ZCODEC, ZREPLAYGAINTRACKGAIN, ZREPLAYGAINTRACKPEAK, "
        "ZREPLAYGAINALBUMGAIN, ZREPLAYGAINALBUMPEAK, ZCURRENT, ZCURRENTPOSITION, "
        "ZQUEUED, ZQUEUEPOSITION, ZSHUFFLEINDEX "
        "from ZPLAYLISTENTRY order by ZINDEX"};
    if (!entries.valid()) {
        // No such table, most likely: a SQLite file that is not a Cog store.
        return std::nullopt;
    }

    while (entries.step()) {
        const std::string urlString = entries.columnText(kUrlString);
        const bool        deleted   = truthy(entries, kDeleted);

        if (pruned(deleted, urlString)) {
            (deleted ? library.prunedDeleted : library.prunedEmptyUrl) += 1;
            continue;
        }

        CogEntry entry;
        entry.fileReference = isCogFileReferenceUrl(urlString);

        // A file reference URL is not a URL this can parse into something
        // useful, but it is also not corruption -- it is a real entry pointing
        // at a real file, in a form only Foundation can resolve. Kept, flagged,
        // and left for a caller that has Foundation.
        if (auto parsed = Url::parse(urlString)) {
            entry.url = *std::move(parsed);
        } else {
            library.prunedUnparseable += 1;
            continue;
        }

        entry.index       = entries.columnInt(kIndex);
        entry.totalFrames = entries.columnInt(kTotalFrames);
        entry.sampleRate  = entries.columnDouble(kSampleRate);
        entry.bitrate     = entries.columnInt(kBitrate);
        entry.channels    = entries.columnInt(kChannels);
        entry.codec       = entries.columnText(kCodec);

        entry.replayGainTrackGain = entries.columnDouble(kTrackGain);
        entry.replayGainTrackPeak = entries.columnDouble(kTrackPeak);
        entry.replayGainAlbumGain = entries.columnDouble(kAlbumGain);
        entry.replayGainAlbumPeak = entries.columnDouble(kAlbumPeak);

        entry.current         = truthy(entries, kCurrent);
        entry.currentPosition = entries.columnDouble(kCurrentPosition);
        entry.queued          = truthy(entries, kQueued);
        entry.queuePosition   = entries.isNull(kQueuePosition)
                                    ? -1
                                    : entries.columnInt(kQueuePosition);
        entry.shuffleIndex    = entries.columnInt(kShuffleIndex);

        if (entry.fileReference) {
            library.fileReferences += 1;
        }
        library.entries.push_back(std::move(entry));
    }

    // Play counts. A store may have none, and that is not a failure -- the
    // table is separate and an untouched Cog install has an empty one.
    std::vector<CogPlayCount> counts;
    if (sql::Statement rows{database,
                               "select ZFILENAME, ZTITLE, ZARTIST, ZALBUM, ZCOUNT, "
                               "ZRATING, ZFIRSTSEEN, ZLASTPLAYED from ZPLAYCOUNT"};
        rows.valid()) {
        while (rows.step()) {
            CogPlayCount count;
            count.filename   = rows.columnText(0);
            count.title      = rows.columnText(1);
            count.artist     = rows.columnText(2);
            count.album      = rows.columnText(3);
            count.count      = rows.columnInt(4);
            count.rating     = rows.columnDouble(5);
            count.firstSeen  = rows.columnDouble(6);
            count.lastPlayed = rows.columnDouble(7);
            counts.push_back(std::move(count));
        }
    }
    library.playCounts = CogPlayCounts{std::move(counts)};

    // The paths, not the bookmarks. See the header for why the blob is left
    // alone: it is opaque, only macOS can resolve it, and the path beside it is
    // the part a non-sandboxed player can act on.
    if (sql::Statement tokens{database,
                                 "select ZPATH from ZSANDBOXTOKEN "
                                 "where ZPATH is not null and ZPATH <> ''"};
        tokens.valid()) {
        while (tokens.step()) {
            library.sandboxPaths.push_back(tokens.columnText(0));
        }
    }

    return library;
}

}  // namespace xpcog
