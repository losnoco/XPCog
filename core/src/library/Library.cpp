#include "xpcog/core/library/Library.hpp"

#include "Sqlite.hpp"
#include "xpcog/core/Sha256.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace xpcog {
namespace {

/// Ordered migrations. Each runs exactly once, inside one transaction with the
/// version bump, so a half-applied schema cannot survive a crash.
///
/// Cog has no equivalent -- Core Data infers lightweight migrations from the
/// model file, which is convenient right up to the point where it cannot, and
/// then the store fails to open with nothing to fix by hand.
constexpr std::array<std::string_view, 1> kMigrations = {
    R"sql(
CREATE TABLE playlist_entry (
    id               INTEGER PRIMARY KEY,
    position         INTEGER NOT NULL,
    url              TEXT    NOT NULL,

    album            TEXT    NOT NULL DEFAULT '',
    album_artist     TEXT    NOT NULL DEFAULT '',
    artist           TEXT    NOT NULL DEFAULT '',
    title            TEXT    NOT NULL DEFAULT '',
    genre            TEXT    NOT NULL DEFAULT '',
    composer         TEXT    NOT NULL DEFAULT '',
    date             TEXT    NOT NULL DEFAULT '',
    comment          TEXT    NOT NULL DEFAULT '',
    unsynced_lyrics  TEXT    NOT NULL DEFAULT '',
    track            INTEGER NOT NULL DEFAULT 0,
    disc             INTEGER NOT NULL DEFAULT 0,
    year             INTEGER NOT NULL DEFAULT 0,
    art_hash         TEXT,

    sample_rate      REAL    NOT NULL DEFAULT 0,
    channels         INTEGER NOT NULL DEFAULT 0,
    channel_config   INTEGER NOT NULL DEFAULT 0,
    sample_format    INTEGER NOT NULL DEFAULT 0,
    bits_per_sample  INTEGER NOT NULL DEFAULT 0,
    big_endian       INTEGER NOT NULL DEFAULT 0,
    total_frames     INTEGER NOT NULL DEFAULT 0,
    bitrate          INTEGER NOT NULL DEFAULT 0,
    seekable         INTEGER NOT NULL DEFAULT 0,
    lossless         INTEGER NOT NULL DEFAULT 0,
    codec            TEXT    NOT NULL DEFAULT '',
    encoding         TEXT    NOT NULL DEFAULT '',
    cuesheet         TEXT,

    rg_track_gain    REAL,
    rg_track_peak    REAL,
    rg_album_gain    REAL,
    rg_album_peak    REAL,
    rg_volume        REAL,
    rg_soundcheck    TEXT,

    metadata_loaded  INTEGER NOT NULL DEFAULT 0,
    error            INTEGER NOT NULL DEFAULT 0,
    error_message    TEXT    NOT NULL DEFAULT '',
    play_count       INTEGER NOT NULL DEFAULT 0,
    current_position REAL    NOT NULL DEFAULT 0,
    stop_after       INTEGER NOT NULL DEFAULT 0,
    shuffle_index    INTEGER NOT NULL DEFAULT -1,
    queue_position   INTEGER NOT NULL DEFAULT -1,
    is_current       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX playlist_entry_position ON playlist_entry(position);
CREATE INDEX playlist_entry_art      ON playlist_entry(art_hash);
CREATE INDEX playlist_entry_url      ON playlist_entry(url);

-- One row per tag value. Cog keeps an NSKeyedArchiver blob per entry and
-- rebuilds a search string from it; a child table makes the same search an
-- indexed query and survives a schema the archiver never anticipated.
CREATE TABLE entry_tag (
    entry_id   INTEGER NOT NULL REFERENCES playlist_entry(id) ON DELETE CASCADE,
    key        TEXT    NOT NULL,
    ordinal    INTEGER NOT NULL,
    value      TEXT,
    value_blob BLOB
);
CREATE INDEX entry_tag_entry ON entry_tag(entry_id);
CREATE INDEX entry_tag_key   ON entry_tag(key, value);

-- Artwork is content-addressed, as in Cog: one copy per distinct image no
-- matter how many tracks of the album are in the playlist.
CREATE TABLE artwork (
    art_hash TEXT PRIMARY KEY,
    data     BLOB NOT NULL
);

CREATE TABLE play_count (
    id          INTEGER PRIMARY KEY,
    artist      TEXT    NOT NULL DEFAULT '',
    album       TEXT    NOT NULL DEFAULT '',
    title       TEXT    NOT NULL DEFAULT '',
    filename    TEXT    NOT NULL DEFAULT '',
    count       INTEGER NOT NULL DEFAULT 0,
    first_seen  INTEGER NOT NULL DEFAULT 0,
    last_played INTEGER NOT NULL DEFAULT 0,
    rating      REAL    NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX play_count_key ON play_count(artist, album, title);

CREATE TABLE playlist_state (
    id      INTEGER PRIMARY KEY CHECK (id = 1),
    repeat  INTEGER NOT NULL DEFAULT 3,
    shuffle INTEGER NOT NULL DEFAULT 0
);
)sql",
};

/// Columns of playlist_entry, in the order both the insert and the select use.
/// One list, so the two cannot drift -- which is the classic way a persistence
/// layer starts writing the artist into the album column.
constexpr std::string_view kEntryColumns =
    "id, position, url, "
    "album, album_artist, artist, title, genre, composer, date, comment, "
    "unsynced_lyrics, track, disc, year, art_hash, "
    "sample_rate, channels, channel_config, sample_format, bits_per_sample, "
    "big_endian, total_frames, bitrate, seekable, lossless, codec, encoding, "
    "cuesheet, "
    "rg_track_gain, rg_track_peak, rg_album_gain, rg_album_peak, rg_volume, "
    "rg_soundcheck, "
    "metadata_loaded, error, error_message, play_count, current_position, "
    "stop_after, shuffle_index, queue_position, is_current";

constexpr int kEntryColumnCount = 44;

void bindOptional(sql::Statement& statement, int index,
                  const std::optional<float>& value) {
    if (value) {
        statement.bind(index, static_cast<double>(*value));
    } else {
        statement.bindNull(index);
    }
}

[[nodiscard]] std::optional<float> readOptionalFloat(const sql::Statement& statement,
                                                     int                   column) {
    if (statement.isNull(column)) {
        return std::nullopt;
    }
    return static_cast<float>(statement.columnDouble(column));
}

}  // namespace

/// The three statements writing an entry takes, prepared once for a whole run.
///
/// Preparing per row is what a straightforward implementation does and it costs
/// more than the inserts: 50k entries went from 3.9 s to well under a second
/// purely by hoisting these out of the loop. sqlite parses SQL, and parsing the
/// same statement 150,000 times is the whole bill.
struct EntryWriter {
    sql::Statement insert;
    sql::Statement clearTags;
    sql::Statement insertTag;

    explicit EntryWriter(sql::Database& database)
        : insert(database, insertSql()),
          clearTags(database, "DELETE FROM entry_tag WHERE entry_id = ?;"),
          insertTag(database,
                    "INSERT INTO entry_tag (entry_id, key, ordinal, value, "
                    "value_blob) VALUES (?,?,?,?,?);") {}

    [[nodiscard]] bool valid() const {
        return insert.valid() && clearTags.valid() && insertTag.valid();
    }

private:
    [[nodiscard]] static std::string insertSql() {
        std::string marks;
        for (int i = 0; i < kEntryColumnCount; ++i) {
            marks += (i == 0) ? "?" : ",?";
        }
        return "INSERT OR REPLACE INTO playlist_entry (" + std::string{kEntryColumns} +
               ") VALUES (" + marks + ");";
    }
};

struct Library::Impl {
    sql::Database database;
    std::string   error;
    int           version = 0;

    [[nodiscard]] bool migrate();
    [[nodiscard]] bool writeEntry(EntryWriter& writer, const PlaylistEntry& entry,
                                  std::int64_t position);
    [[nodiscard]] bool writeTags(EntryWriter& writer, const PlaylistEntry& entry);
    [[nodiscard]] bool readEntries(std::vector<PlaylistEntry>& entries,
                                   std::optional<TrackId>&     current);
    [[nodiscard]] bool readTags(sql::Statement& query, PlaylistEntry& entry);

    bool fail(std::string message) {
        error = std::move(message) + ": " + database.lastError();
        return false;
    }
};

Library::Library() : impl_(std::make_unique<Impl>()) {}
Library::~Library()                            = default;
Library::Library(Library&&) noexcept            = default;
Library& Library::operator=(Library&&) noexcept = default;

bool Library::open(const std::filesystem::path& path) {
    if (!impl_->database.open(path)) {
        return impl_->fail("cannot open " + path.string());
    }
    return impl_->migrate();
}

void Library::close() { impl_->database.close(); }
bool Library::isOpen() const { return impl_->database.isOpen(); }
std::string Library::lastError() const { return impl_->error; }
int Library::schemaVersion() const { return impl_->version; }

bool Library::Impl::migrate() {
    if (!database.exec("CREATE TABLE IF NOT EXISTS schema_version "
                       "(version INTEGER NOT NULL);")) {
        return fail("cannot create schema_version");
    }

    version = 0;
    {
        sql::Statement query{database, "SELECT version FROM schema_version;"};
        if (query.step()) {
            version = static_cast<int>(query.columnInt(0));
        }
    }

    const int target = static_cast<int>(kMigrations.size());
    if (version >= target) {
        return true;
    }

    sql::Transaction transaction{database};
    for (int step = version; step < target; ++step) {
        if (!database.exec(kMigrations[static_cast<std::size_t>(step)])) {
            return fail("migration " + std::to_string(step + 1) + " failed");
        }
    }
    if (!database.exec("DELETE FROM schema_version;") ||
        !database.exec("INSERT INTO schema_version (version) VALUES (" +
                       std::to_string(target) + ");")) {
        return fail("cannot record schema version");
    }
    if (!transaction.commit()) {
        return fail("cannot commit migrations");
    }

    version = target;
    return true;
}

// --- entries ------------------------------------------------------------

bool Library::Impl::writeEntry(EntryWriter& writer, const PlaylistEntry& entry,
                               std::int64_t position) {
    sql::Statement& statement = writer.insert;
    statement.reset();

    const AudioFormat& format = entry.properties.format;
    const ReplayGainInfo& gain = entry.properties.replayGain;

    int i = 1;
    statement.bind(i++, static_cast<std::int64_t>(entry.id));
    statement.bind(i++, position);
    statement.bind(i++, entry.url.toString());

    statement.bind(i++, entry.album);
    statement.bind(i++, entry.albumArtist);
    statement.bind(i++, entry.artist);
    statement.bind(i++, entry.rawTitle);
    statement.bind(i++, entry.genre);
    statement.bind(i++, entry.composer);
    statement.bind(i++, entry.date);
    statement.bind(i++, entry.comment);
    statement.bind(i++, entry.unsyncedLyrics);
    statement.bind(i++, static_cast<std::int64_t>(entry.track));
    statement.bind(i++, static_cast<std::int64_t>(entry.disc));
    statement.bind(i++, static_cast<std::int64_t>(entry.year));

    // Artwork lives in its own table; the entry keeps only the hash, which
    // storeArtwork() hands back.
    if (entry.artHash.empty()) {
        statement.bindNull(i++);
    } else {
        statement.bind(i++, entry.artHash);
    }

    statement.bind(i++, format.sampleRate);
    statement.bind(i++, static_cast<std::int64_t>(format.channels));
    statement.bind(i++, static_cast<std::int64_t>(format.channelConfig));
    statement.bind(i++, static_cast<std::int64_t>(format.format));
    statement.bind(i++, static_cast<std::int64_t>(format.bitsPerSample));
    statement.bind(i++, static_cast<std::int64_t>(format.bigEndian ? 1 : 0));
    statement.bind(i++, entry.properties.totalFrames);
    statement.bind(i++, static_cast<std::int64_t>(entry.properties.bitrateKbps));
    statement.bind(i++, static_cast<std::int64_t>(entry.properties.seekable ? 1 : 0));
    statement.bind(i++, static_cast<std::int64_t>(entry.properties.lossless ? 1 : 0));
    statement.bind(i++, entry.properties.codec);
    statement.bind(i++, entry.properties.encoding);
    if (entry.properties.cuesheet) {
        statement.bind(i++, *entry.properties.cuesheet);
    } else {
        statement.bindNull(i++);
    }

    bindOptional(statement, i++, gain.trackGain);
    bindOptional(statement, i++, gain.trackPeak);
    bindOptional(statement, i++, gain.albumGain);
    bindOptional(statement, i++, gain.albumPeak);
    bindOptional(statement, i++, gain.volume);
    if (gain.soundcheck) {
        statement.bind(i++, *gain.soundcheck);
    } else {
        statement.bindNull(i++);
    }

    statement.bind(i++, static_cast<std::int64_t>(entry.metadataLoaded ? 1 : 0));
    statement.bind(i++, static_cast<std::int64_t>(entry.error ? 1 : 0));
    statement.bind(i++, entry.errorMessage);
    statement.bind(i++, entry.playCount);
    statement.bind(i++, entry.currentPosition);
    statement.bind(i++, static_cast<std::int64_t>(entry.stopAfter ? 1 : 0));
    statement.bind(i++, entry.shuffleIndex);
    statement.bind(i++, static_cast<std::int64_t>(entry.queuePosition));
    statement.bind(i++, static_cast<std::int64_t>(0));  // is_current, set below

    if (!statement.run()) {
        return fail("cannot write entry " + entry.url.toString());
    }
    return writeTags(writer, entry);
}

bool Library::Impl::writeTags(EntryWriter& writer, const PlaylistEntry& entry) {
    writer.clearTags.reset();
    writer.clearTags.bind(1, static_cast<std::int64_t>(entry.id));
    if (!writer.clearTags.run()) {
        return fail("cannot clear tags");
    }

    sql::Statement& insert = writer.insertTag;
    for (const auto& [key, value] : entry.metadata) {
        if (const auto* strings = std::get_if<std::vector<std::string>>(&value)) {
            for (std::size_t ordinal = 0; ordinal < strings->size(); ++ordinal) {
                insert.reset();
                insert.bind(1, static_cast<std::int64_t>(entry.id));
                insert.bind(2, key);
                insert.bind(3, static_cast<std::int64_t>(ordinal));
                insert.bind(4, (*strings)[ordinal]);
                insert.bindNull(5);
                if (!insert.run()) {
                    return fail("cannot write tag " + key);
                }
            }
        } else if (const auto* bytes = std::get_if<std::vector<std::byte>>(&value)) {
            insert.reset();
            insert.bind(1, static_cast<std::int64_t>(entry.id));
            insert.bind(2, key);
            insert.bind(3, static_cast<std::int64_t>(0));
            insert.bindNull(4);
            insert.bind(5, std::span<const std::byte>{*bytes});
            if (!insert.run()) {
                return fail("cannot write binary tag " + key);
            }
        }
    }
    return true;
}

bool Library::Impl::readTags(sql::Statement& query, PlaylistEntry& entry) {
    query.reset();
    query.bind(1, static_cast<std::int64_t>(entry.id));

    while (query.step()) {
        const std::string key = query.columnText(0);
        if (query.isNull(1)) {
            entry.metadata.setBytes(key, query.columnBlob(2));
        } else {
            // add() rather than set(): ordinal ordering is what makes a
            // multi-value tag come back in the order it was written.
            entry.metadata.add(key, query.columnText(1));
        }
    }
    return true;
}

bool Library::Impl::readEntries(std::vector<PlaylistEntry>& entries,
                                std::optional<TrackId>&     current) {
    sql::Statement query{database, "SELECT " + std::string{kEntryColumns} +
                                       " FROM playlist_entry ORDER BY position;"};
    if (!query.valid()) {
        return fail("cannot prepare entry select");
    }

    while (query.step()) {
        PlaylistEntry entry;
        int           i = 0;

        entry.id = static_cast<TrackId>(query.columnInt(i++));
        ++i;  // position: implied by row order
        if (const auto url = Url::parse(query.columnText(i++))) {
            entry.url = *url;
        }

        entry.album          = query.columnText(i++);
        entry.albumArtist    = query.columnText(i++);
        entry.artist         = query.columnText(i++);
        entry.rawTitle       = query.columnText(i++);
        entry.genre          = query.columnText(i++);
        entry.composer       = query.columnText(i++);
        entry.date           = query.columnText(i++);
        entry.comment        = query.columnText(i++);
        entry.unsyncedLyrics = query.columnText(i++);
        entry.track          = static_cast<std::int32_t>(query.columnInt(i++));
        entry.disc           = static_cast<std::int32_t>(query.columnInt(i++));
        entry.year           = static_cast<std::int32_t>(query.columnInt(i++));
        entry.artHash = query.columnText(i++);

        AudioFormat& format  = entry.properties.format;
        format.sampleRate    = query.columnDouble(i++);
        format.channels      = static_cast<std::uint32_t>(query.columnInt(i++));
        format.channelConfig = static_cast<std::uint32_t>(query.columnInt(i++));
        format.format        = static_cast<SampleFormat>(query.columnInt(i++));
        format.bitsPerSample = static_cast<std::uint32_t>(query.columnInt(i++));
        format.bigEndian     = query.columnInt(i++) != 0;

        entry.properties.totalFrames = query.columnInt(i++);
        entry.properties.bitrateKbps = static_cast<std::int32_t>(query.columnInt(i++));
        entry.properties.seekable    = query.columnInt(i++) != 0;
        entry.properties.lossless    = query.columnInt(i++) != 0;
        entry.properties.codec       = query.columnText(i++);
        entry.properties.encoding    = query.columnText(i++);
        if (!query.isNull(i)) {
            entry.properties.cuesheet = query.columnText(i);
        }
        ++i;

        ReplayGainInfo& gain = entry.properties.replayGain;
        gain.trackGain       = readOptionalFloat(query, i++);
        gain.trackPeak       = readOptionalFloat(query, i++);
        gain.albumGain       = readOptionalFloat(query, i++);
        gain.albumPeak       = readOptionalFloat(query, i++);
        gain.volume          = readOptionalFloat(query, i++);
        if (!query.isNull(i)) {
            gain.soundcheck = query.columnText(i);
        }
        ++i;

        entry.metadataLoaded  = query.columnInt(i++) != 0;
        entry.error           = query.columnInt(i++) != 0;
        entry.errorMessage    = query.columnText(i++);
        entry.playCount       = query.columnInt(i++);
        entry.currentPosition = query.columnDouble(i++);
        entry.stopAfter       = query.columnInt(i++) != 0;
        entry.shuffleIndex    = query.columnInt(i++);
        entry.queuePosition   = static_cast<std::int32_t>(query.columnInt(i++));
        if (query.columnInt(i++) != 0) {
            current = entry.id;
        }

        entries.push_back(std::move(entry));
    }

    sql::Statement tags{database,
                        "SELECT key, value, value_blob FROM entry_tag "
                        "WHERE entry_id = ? ORDER BY key, ordinal;"};
    if (!tags.valid()) {
        return fail("cannot prepare the tag select");
    }
    for (auto& entry : entries) {
        if (!readTags(tags, entry)) {
            return false;
        }
    }
    return true;
}

// --- playlist -----------------------------------------------------------

bool Library::savePlaylist(const Playlist& playlist) {
    if (!isOpen()) {
        return impl_->fail("no database");
    }

    const Playlist::Snapshot state = playlist.snapshot();

    sql::Transaction transaction{impl_->database};
    if (!impl_->database.exec("DELETE FROM playlist_entry;")) {
        return impl_->fail("cannot clear playlist");
    }

    EntryWriter writer{impl_->database};
    if (!writer.valid()) {
        return impl_->fail("cannot prepare the entry statements");
    }
    for (std::size_t position = 0; position < state.entries.size(); ++position) {
        if (!impl_->writeEntry(writer, state.entries[position],
                               static_cast<std::int64_t>(position))) {
            return false;
        }
    }

    if (state.current) {
        sql::Statement mark{impl_->database,
                            "UPDATE playlist_entry SET is_current = 1 WHERE id = ?;"};
        mark.bind(1, static_cast<std::int64_t>(*state.current));
        if (!mark.run()) {
            return impl_->fail("cannot mark the playing entry");
        }
    }

    // The shuffle order is stored per entry rather than as a list, exactly as
    // Cog does: a list can hold the same entry more than once once it has been
    // extended past a full pass, and those repeats are not worth persisting.
    {
        sql::Statement clear{impl_->database,
                             "UPDATE playlist_entry SET shuffle_index = -1;"};
        if (!clear.run()) {
            return impl_->fail("cannot clear the shuffle order");
        }
        sql::Statement mark{impl_->database,
                            "UPDATE playlist_entry SET shuffle_index = ? WHERE id = ?;"};
        for (std::size_t i = 0; i < state.shuffleOrder.size(); ++i) {
            mark.reset();
            mark.bind(1, static_cast<std::int64_t>(i));
            mark.bind(2, static_cast<std::int64_t>(state.shuffleOrder[i]));
            if (!mark.run()) {
                return impl_->fail("cannot write the shuffle order");
            }
        }
    }

    {
        sql::Statement modes{impl_->database,
                             "INSERT OR REPLACE INTO playlist_state (id, repeat, "
                             "shuffle) VALUES (1, ?, ?);"};
        modes.bind(1, static_cast<std::int64_t>(state.repeat));
        modes.bind(2, static_cast<std::int64_t>(state.shuffle));
        if (!modes.run()) {
            return impl_->fail("cannot write playback modes");
        }
    }

    if (!transaction.commit()) {
        return impl_->fail("cannot commit the playlist");
    }
    impl_->error.clear();
    return true;
}

bool Library::loadPlaylist(Playlist& playlist) {
    if (!isOpen()) {
        return impl_->fail("no database");
    }

    Playlist::Snapshot state;
    if (!impl_->readEntries(state.entries, state.current)) {
        return false;
    }

    // Queue and shuffle order are rebuilt by sorting on the columns, so the two
    // lists cannot disagree with the per-entry values the UI displays.
    std::vector<const PlaylistEntry*> queued;
    std::vector<const PlaylistEntry*> shuffled;
    for (const auto& entry : state.entries) {
        if (entry.queuePosition >= 0) {
            queued.push_back(&entry);
        }
        if (entry.shuffleIndex >= 0) {
            shuffled.push_back(&entry);
        }
    }
    std::sort(queued.begin(), queued.end(),
              [](const PlaylistEntry* a, const PlaylistEntry* b) {
                  return a->queuePosition < b->queuePosition;
              });
    std::sort(shuffled.begin(), shuffled.end(),
              [](const PlaylistEntry* a, const PlaylistEntry* b) {
                  return a->shuffleIndex < b->shuffleIndex;
              });
    for (const PlaylistEntry* entry : queued) {
        state.queue.push_back(entry->id);
    }
    for (const PlaylistEntry* entry : shuffled) {
        state.shuffleOrder.push_back(entry->id);
    }

    {
        sql::Statement modes{impl_->database,
                             "SELECT repeat, shuffle FROM playlist_state WHERE id = 1;"};
        if (modes.step()) {
            state.repeat  = static_cast<RepeatMode>(modes.columnInt(0));
            state.shuffle = static_cast<ShuffleMode>(modes.columnInt(1));
        }
    }

    playlist.restore(std::move(state));
    impl_->error.clear();
    return true;
}

bool Library::saveEntry(const PlaylistEntry& entry) {
    if (!isOpen()) {
        return impl_->fail("no database");
    }

    // The row has to keep its place, and the caller does not know it. Reading it
    // back is cheaper than making every caller thread a position through.
    std::int64_t position = 0;
    {
        sql::Statement query{impl_->database,
                             "SELECT position FROM playlist_entry WHERE id = ?;"};
        query.bind(1, static_cast<std::int64_t>(entry.id));
        if (query.step()) {
            position = query.columnInt(0);
        } else {
            sql::Statement last{impl_->database,
                                "SELECT COALESCE(MAX(position) + 1, 0) FROM "
                                "playlist_entry;"};
            if (last.step()) {
                position = last.columnInt(0);
            }
        }
    }

    sql::Transaction transaction{impl_->database};
    EntryWriter      writer{impl_->database};
    if (!writer.valid()) {
        return impl_->fail("cannot prepare the entry statements");
    }
    if (!impl_->writeEntry(writer, entry, position)) {
        return false;
    }
    if (!transaction.commit()) {
        return impl_->fail("cannot commit the entry");
    }
    impl_->error.clear();
    return true;
}

// --- artwork ------------------------------------------------------------

std::string Library::storeArtwork(std::span<const std::byte> data) {
    if (!isOpen() || data.empty()) {
        return {};
    }

    const std::string hash = sha256Hex(data);

    sql::Statement insert{impl_->database,
                          "INSERT OR IGNORE INTO artwork (art_hash, data) "
                          "VALUES (?, ?);"};
    insert.bind(1, hash);
    insert.bind(2, data);
    if (!insert.run()) {
        static_cast<void>(impl_->fail("cannot store artwork"));
        return {};
    }
    return hash;
}

std::vector<std::byte> Library::artwork(std::string_view hash) const {
    if (!isOpen() || hash.empty()) {
        return {};
    }
    sql::Statement query{impl_->database, "SELECT data FROM artwork WHERE art_hash = ?;"};
    query.bind(1, hash);
    if (!query.step()) {
        return {};
    }
    return query.columnBlob(0);
}

std::int64_t Library::pruneArtwork() {
    if (!isOpen()) {
        return 0;
    }
    if (!impl_->database.exec(
            "DELETE FROM artwork WHERE art_hash NOT IN "
            "(SELECT art_hash FROM playlist_entry WHERE art_hash IS NOT NULL);")) {
        static_cast<void>(impl_->fail("cannot prune artwork"));
        return 0;
    }
    return sqlite3_changes(impl_->database.handle());
}

// --- play counts --------------------------------------------------------

std::optional<PlayCountRecord> Library::playCount(std::string_view artist,
                                                  std::string_view album,
                                                  std::string_view title) const {
    if (!isOpen()) {
        return std::nullopt;
    }

    sql::Statement query{impl_->database,
                         "SELECT artist, album, title, filename, count, first_seen, "
                         "last_played, rating FROM play_count "
                         "WHERE artist = ? AND album = ? AND title = ?;"};
    query.bind(1, artist);
    query.bind(2, album);
    query.bind(3, title);
    if (!query.step()) {
        return std::nullopt;
    }

    PlayCountRecord record;
    record.artist     = query.columnText(0);
    record.album      = query.columnText(1);
    record.title      = query.columnText(2);
    record.filename   = query.columnText(3);
    record.count      = query.columnInt(4);
    record.firstSeen  = query.columnInt(5);
    record.lastPlayed = query.columnInt(6);
    record.rating     = static_cast<float>(query.columnDouble(7));
    return record;
}

bool Library::recordPlay(const PlaylistEntry& entry, std::int64_t whenUnixSeconds) {
    if (!isOpen()) {
        return impl_->fail("no database");
    }

    // Upsert on the natural key, so playing the same track from two different
    // files (a rip and a re-rip, or a cue track and the flat file) shares one
    // count -- which is the behaviour Cog's three-predicate fetch produces.
    sql::Statement statement{
        impl_->database,
        "INSERT INTO play_count (artist, album, title, filename, count, "
        "first_seen, last_played) VALUES (?,?,?,?,1,?,?) "
        "ON CONFLICT(artist, album, title) DO UPDATE SET "
        "count = count + 1, last_played = excluded.last_played, "
        "filename = excluded.filename;"};
    if (!statement.valid()) {
        return impl_->fail("cannot prepare the play count upsert");
    }

    statement.bind(1, entry.artist);
    statement.bind(2, entry.album);
    statement.bind(3, entry.title());
    statement.bind(4, entry.filename());
    statement.bind(5, whenUnixSeconds);
    statement.bind(6, whenUnixSeconds);
    if (!statement.run()) {
        return impl_->fail("cannot record the play");
    }
    impl_->error.clear();
    return true;
}

bool Library::setRating(const PlaylistEntry& entry, float rating) {
    if (!isOpen()) {
        return impl_->fail("no database");
    }

    sql::Statement statement{
        impl_->database,
        "INSERT INTO play_count (artist, album, title, filename, rating) "
        "VALUES (?,?,?,?,?) "
        "ON CONFLICT(artist, album, title) DO UPDATE SET rating = excluded.rating;"};
    statement.bind(1, entry.artist);
    statement.bind(2, entry.album);
    statement.bind(3, entry.title());
    statement.bind(4, entry.filename());
    statement.bind(5, static_cast<double>(rating));
    if (!statement.run()) {
        return impl_->fail("cannot set the rating");
    }
    impl_->error.clear();
    return true;
}

}  // namespace xpcog
