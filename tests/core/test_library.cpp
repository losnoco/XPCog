// Persistence tests: what Core Data used to do, and what the port must not lose
// on the way to SQLite.

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Sha256.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

Library openMemoryLibrary() {
    Library library;
    REQUIRE(library.open(":memory:"));
    return library;
}

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> data;
    data.reserve(text.size());
    for (const char character : text) {
        data.push_back(static_cast<std::byte>(character));
    }
    return data;
}

PlaylistEntry fullyPopulatedEntry() {
    PlaylistEntry entry;
    entry.url         = Url::fromLocalPath("/music/Pink Floyd/Animals/2 Dogs.flac");
    entry.album       = "Animals";
    entry.albumArtist = "Pink Floyd";
    entry.artist      = "Pink Floyd";
    entry.rawTitle    = "Dogs";
    entry.genre       = "Progressive Rock";
    entry.composer    = "Roger Waters";
    entry.date        = "1977-01-23";
    entry.comment     = "32DP-360";
    entry.unsyncedLyrics = "You gotta be crazy";
    entry.track       = 2;
    entry.disc        = 1;
    entry.year        = 1977;
    entry.artHash     = std::string(64, 'a');

    entry.metadata.set("mood", std::vector<std::string>{"bleak", "restless"});
    entry.metadata.set("isrc", "GBAYE7700123");
    entry.metadata.setBytes("waveform", bytesOf("\x01\x02\x03"));

    entry.properties.format.sampleRate    = 44100.0;
    entry.properties.format.channels      = 2;
    entry.properties.format.channelConfig = 3;
    entry.properties.format.format        = SampleFormat::S16;
    entry.properties.format.bitsPerSample = 16;
    entry.properties.totalFrames          = 44100 * 1023;
    entry.properties.bitrateKbps          = 900;
    entry.properties.seekable             = true;
    entry.properties.lossless             = true;
    entry.properties.codec                = "FLAC";
    entry.properties.encoding             = "lossless";
    entry.properties.cuesheet             = "REM one line";

    entry.properties.replayGain.trackGain  = -3.5F;
    entry.properties.replayGain.trackPeak  = 0.98F;
    entry.properties.replayGain.albumGain  = -4.25F;
    entry.properties.replayGain.soundcheck = "00000E4D";

    entry.metadataLoaded  = true;
    entry.playCount       = 7;
    entry.currentPosition = 123.5;
    return entry;
}

}  // namespace

TEST_CASE("a new database migrates to the current schema", "[library]") {
    const Library library = openMemoryLibrary();
    REQUIRE(library.isOpen());
    REQUIRE(library.schemaVersion() == 3);
}

TEST_CASE("migrations are not reapplied", "[library]") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "xpcog-migrate-test.db";
    std::filesystem::remove(path);

    {
        Library library;
        REQUIRE(library.open(path));
        REQUIRE(library.schemaVersion() == 3);
    }
    {
        // Reopening must not try to CREATE TABLE again, which would fail.
        Library library;
        REQUIRE(library.open(path));
        REQUIRE(library.schemaVersion() == 3);
        REQUIRE(library.lastError().empty());
    }

    std::filesystem::remove(path);
}

TEST_CASE("a playlist round-trips through the database", "[library]") {
    Library library = openMemoryLibrary();

    Playlist saved;
    const TrackId first = saved.add(fullyPopulatedEntry());

    PlaylistEntry second;
    second.url      = Url::fromLocalPath("/music/Pink Floyd/Animals/3 Pigs.flac");
    second.rawTitle = "Pigs (Three Different Ones)";
    second.album    = "Animals";
    second.track    = 3;
    const TrackId secondId = saved.add(std::move(second));

    saved.setCurrent(first);
    saved.enqueue(secondId);
    saved.setRepeat(RepeatMode::Album);

    REQUIRE(library.savePlaylist(saved));

    Playlist loaded;
    REQUIRE(library.loadPlaylist(loaded));

    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded.current() == first);
    REQUIRE(loaded.queue() == std::vector<TrackId>{secondId});
    REQUIRE(loaded.repeat() == RepeatMode::Album);

    const PlaylistEntry& entry = loaded.at(0);
    REQUIRE(entry.id == first);
    REQUIRE(entry.url == fullyPopulatedEntry().url);
    REQUIRE(entry.album == "Animals");
    REQUIRE(entry.albumArtist == "Pink Floyd");
    REQUIRE(entry.composer == "Roger Waters");
    REQUIRE(entry.unsyncedLyrics == "You gotta be crazy");
    REQUIRE(entry.track == 2);
    REQUIRE(entry.year == 1977);
    REQUIRE(entry.artHash == std::string(64, 'a'));

    REQUIRE(entry.properties.format.sampleRate == 44100.0);
    REQUIRE(entry.properties.format.format == SampleFormat::S16);
    REQUIRE(entry.properties.totalFrames == 44100 * 1023);
    REQUIRE(entry.properties.lossless);
    REQUIRE(entry.properties.codec == "FLAC");
    REQUIRE(entry.properties.cuesheet == "REM one line");

    // ReplayGain fields are optional, and "absent" has to survive as absent --
    // a 0.0 album peak would silence a track under albumGainWithPeak.
    REQUIRE(entry.properties.replayGain.trackGain == -3.5F);
    REQUIRE(entry.properties.replayGain.albumGain == -4.25F);
    REQUIRE_FALSE(entry.properties.replayGain.albumPeak.has_value());
    REQUIRE(entry.properties.replayGain.soundcheck == "00000E4D");

    REQUIRE(entry.playCount == 7);
    REQUIRE(entry.currentPosition == 123.5);
}

TEST_CASE("unpromoted tags survive with their multiple values", "[library]") {
    Library library = openMemoryLibrary();

    Playlist saved;
    saved.add(fullyPopulatedEntry());
    REQUIRE(library.savePlaylist(saved));

    Playlist loaded;
    REQUIRE(library.loadPlaylist(loaded));

    const MetadataMap& tags = loaded.at(0).metadata;
    REQUIRE(tags.joined("mood") == "bleak, restless");
    REQUIRE(tags.first("isrc") == "GBAYE7700123");

    // Binary tag values are what Cog's NSKeyedArchiver blob existed to carry;
    // a BLOB column carries them without the archiver.
    const auto* waveform = tags.bytes("waveform");
    REQUIRE(waveform != nullptr);
    REQUIRE(*waveform == bytesOf("\x01\x02\x03"));
}

TEST_CASE("the shuffle order survives a restart", "[library]") {
    Library library = openMemoryLibrary();

    Playlist saved;
    saved.seedShuffle(1234);
    for (int i = 0; i < 8; ++i) {
        PlaylistEntry entry;
        entry.url      = Url::fromLocalPath(pathFromUtf8("/music/" + std::to_string(i) + ".flac"));
        entry.rawTitle = std::to_string(i);
        saved.add(std::move(entry));
    }
    saved.setShuffle(ShuffleMode::All);
    saved.setCurrent(saved.at(3).id);

    const auto before = saved.snapshot().shuffleOrder;
    REQUIRE(library.savePlaylist(saved));

    Playlist loaded;
    REQUIRE(library.loadPlaylist(loaded));

    REQUIRE(loaded.shuffle() == ShuffleMode::All);
    REQUIRE(loaded.snapshot().shuffleOrder == before);
    REQUIRE(loaded.current() == saved.current());
}

TEST_CASE("saving replaces rather than accumulating", "[library]") {
    Library library = openMemoryLibrary();

    Playlist playlist;
    PlaylistEntry entry;
    entry.url = Url::fromLocalPath("/music/one.flac");
    playlist.add(std::move(entry));

    REQUIRE(library.savePlaylist(playlist));
    REQUIRE(library.savePlaylist(playlist));

    Playlist loaded;
    REQUIRE(library.loadPlaylist(loaded));
    REQUIRE(loaded.size() == 1);
}

TEST_CASE("artwork is stored once per distinct image", "[library]") {
    Library library = openMemoryLibrary();

    const std::vector<std::byte> cover = bytesOf("PNG-ish bytes for the cover");
    const std::string            hash  = library.storeArtwork(cover);

    REQUIRE(hash.size() == 64);
    REQUIRE(hash == sha256Hex(cover));
    REQUIRE(library.storeArtwork(cover) == hash);  // idempotent
    REQUIRE(library.artwork(hash) == cover);

    REQUIRE(library.artwork("nothing stored under this").empty());
}

TEST_CASE("text shared between entries survives the dictionary", "[library]") {
    // Repeated text is stored once and referred to by id, so the interesting
    // case is the second entry: its strings come back from the write cache
    // rather than the table, and a mix-up there would give one entry another's
    // album. Multi-value tags matter too -- ordinal ordering has to survive the
    // indirection, and the tag select now sorts by the dictionary's text.
    Library library = openMemoryLibrary();

    Playlist playlist;
    for (int i = 0; i < 3; ++i) {
        PlaylistEntry entry;
        entry.url         = Url::fromLocalPath(
            pathFromUtf8("/music/shared/" + std::to_string(i) + ".flac"));
        entry.album       = "One Album";
        entry.albumArtist = "One Artist";
        entry.comment     = std::string(4096, 'c');  // the kind of thing worth sharing
        entry.rawTitle    = "Track " + std::to_string(i);
        entry.metadata.add("performer", "First");
        entry.metadata.add("performer", "Second");
        entry.metadata.add("performer", "Third");
        playlist.add(std::move(entry));
    }
    REQUIRE(library.savePlaylist(playlist));

    Playlist restored;
    REQUIRE(library.loadPlaylist(restored));
    REQUIRE(restored.size() == 3);

    for (std::size_t i = 0; i < restored.size(); ++i) {
        const PlaylistEntry& entry = restored.at(i);
        CHECK(entry.album == "One Album");
        CHECK(entry.albumArtist == "One Artist");
        CHECK(entry.comment == std::string(4096, 'c'));
        CHECK(entry.rawTitle == "Track " + std::to_string(i));

        CHECK(entry.metadata.joined("performer", "|") == "First|Second|Third");
    }
}

TEST_CASE("saving twice keeps tags that did not change", "[library]") {
    // The trap this is here for: the second save decides an entry's tags have
    // not moved and leaves the rows alone, which is only safe if rewriting the
    // entry row leaves them alone too. INSERT OR REPLACE does not -- it deletes
    // the row it replaces, and entry_tag's foreign key cascades on delete, so
    // the tags would go and nothing would put them back. Silent, and it would
    // look like the tags were never written.
    Library library = openMemoryLibrary();

    Playlist playlist;
    PlaylistEntry entry;
    entry.url   = Url::fromLocalPath(pathFromUtf8("/music/twice.flac"));
    entry.album = "An Album";
    entry.metadata.add("performer", "First");
    entry.metadata.add("performer", "Second");
    playlist.add(std::move(entry));

    REQUIRE(library.savePlaylist(playlist));
    REQUIRE(library.savePlaylist(playlist));

    Playlist restored;
    REQUIRE(library.loadPlaylist(restored));
    REQUIRE(restored.size() == 1);
    CHECK(restored.at(0).album == "An Album");
    CHECK(restored.at(0).metadata.joined("performer", "|") == "First|Second");
}

TEST_CASE("a tag edit between saves is not skipped", "[library]") {
    // The opposite failure: skipping too much. The fingerprint has to notice a
    // value changing, a value being added, and a key going away.
    Library library = openMemoryLibrary();

    Playlist playlist;
    PlaylistEntry entry;
    entry.url = Url::fromLocalPath(pathFromUtf8("/music/edited.flac"));
    entry.metadata.add("performer", "First");
    const TrackId id = playlist.add(std::move(entry));
    REQUIRE(library.savePlaylist(playlist));

    playlist.update(id, [](PlaylistEntry& target) {
        target.metadata.set("performer", std::vector<std::string>{"Changed", "Added"});
        target.metadata.set("mood", std::vector<std::string>{"New Key"});
    });
    REQUIRE(library.savePlaylist(playlist));

    Playlist restored;
    REQUIRE(library.loadPlaylist(restored));
    REQUIRE(restored.size() == 1);
    CHECK(restored.at(0).metadata.joined("performer", "|") == "Changed|Added");
    CHECK(restored.at(0).metadata.joined("mood") == "New Key");
}

TEST_CASE("an entry removed between saves is removed from the file", "[library]") {
    // The delete is no longer "clear the table", so what goes has to be worked
    // out rather than assumed -- and what stays has to keep its tags.
    Library library = openMemoryLibrary();

    Playlist playlist;
    std::vector<TrackId> ids;
    for (int i = 0; i < 3; ++i) {
        PlaylistEntry entry;
        entry.url = Url::fromLocalPath(
            pathFromUtf8("/music/gone" + std::to_string(i) + ".flac"));
        entry.metadata.add("performer", "Number " + std::to_string(i));
        ids.push_back(playlist.add(std::move(entry)));
    }
    REQUIRE(library.savePlaylist(playlist));

    playlist.remove({ids[1]});
    REQUIRE(library.savePlaylist(playlist));

    Playlist restored;
    REQUIRE(library.loadPlaylist(restored));
    REQUIRE(restored.size() == 2);
    CHECK(restored.at(0).metadata.joined("performer") == "Number 0");
    CHECK(restored.at(1).metadata.joined("performer") == "Number 2");
}

TEST_CASE("unreferenced artwork is pruned", "[library]") {
    Library library = openMemoryLibrary();

    const std::string kept    = library.storeArtwork(bytesOf("kept cover"));
    const std::string orphan  = library.storeArtwork(bytesOf("orphaned cover"));

    Playlist playlist;
    PlaylistEntry entry;
    entry.url     = Url::fromLocalPath("/music/one.flac");
    entry.artHash = kept;
    playlist.add(std::move(entry));
    REQUIRE(library.savePlaylist(playlist));

    // Saving prunes: the transaction that decides which covers are referenced is
    // the right place to drop the ones that are not, and leaving it to a caller
    // meant nobody did it. So by here the orphan is already gone, and the
    // explicit sweep -- still public, and still what a caller would reach for
    // outside a save -- has nothing left to find.
    REQUIRE_FALSE(library.artwork(kept).empty());
    REQUIRE(library.artwork(orphan).empty());
    CHECK(library.pruneArtwork() == 0);
}

TEST_CASE("play counts accumulate on the natural key", "[library]") {
    Library library = openMemoryLibrary();

    PlaylistEntry entry;
    entry.url      = Url::fromLocalPath("/music/dogs.flac");
    entry.artist   = "Pink Floyd";
    entry.album    = "Animals";
    entry.rawTitle = "Dogs";

    REQUIRE(library.recordPlay(entry, 1000));
    REQUIRE(library.recordPlay(entry, 2000));

    // A different file of the same recording shares the count, which is what
    // Cog's three-predicate fetch produces -- here it is one indexed lookup.
    PlaylistEntry rerip = entry;
    rerip.url          = Url::fromLocalPath("/music/rerip/02 Dogs.flac");
    REQUIRE(library.recordPlay(rerip, 3000));

    const auto record = library.playCount("Pink Floyd", "Animals", "Dogs");
    REQUIRE(record.has_value());
    REQUIRE(record->count == 3);
    REQUIRE(record->firstSeen == 1000);
    REQUIRE(record->lastPlayed == 3000);
    REQUIRE(record->filename == "02 Dogs.flac");

    REQUIRE_FALSE(library.playCount("Pink Floyd", "Animals", "Sheep").has_value());
}

TEST_CASE("ratings attach to the play count row", "[library]") {
    Library library = openMemoryLibrary();

    PlaylistEntry entry;
    entry.url      = Url::fromLocalPath("/music/sheep.flac");
    entry.artist   = "Pink Floyd";
    entry.album    = "Animals";
    entry.rawTitle = "Sheep";

    REQUIRE(library.setRating(entry, 4.5F));
    REQUIRE(library.recordPlay(entry, 500));

    const auto record = library.playCount("Pink Floyd", "Animals", "Sheep");
    REQUIRE(record.has_value());
    REQUIRE(record->rating == 4.5F);
    REQUIRE(record->count == 1);  // rating first, then a play: neither clobbers
}

TEST_CASE("a large playlist saves in one transaction", "[library]") {
    Library library = openMemoryLibrary();

    Playlist playlist;
    std::vector<PlaylistEntry> entries;
    entries.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        PlaylistEntry entry;
        entry.url      = Url::fromLocalPath(pathFromUtf8("/music/" + std::to_string(i) + ".flac"));
        entry.rawTitle = "Track " + std::to_string(i);
        entry.album    = "Album " + std::to_string(i / 12);
        entry.track    = (i % 12) + 1;
        entry.properties.format.sampleRate = 44100.0;
        entry.properties.totalFrames       = 44100 * 200;
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));

    const auto start = std::chrono::steady_clock::now();
    REQUIRE(library.savePlaylist(playlist));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    // ~1.2 s in a Debug build on an M-series Mac. The ceiling is loose because
    // it exists to catch a per-row commit or an accidental O(n^2) -- both of
    // which take minutes -- not to police wall-clock on a loaded CI runner.
    REQUIRE(elapsed < 20000);

    Playlist loaded;
    REQUIRE(library.loadPlaylist(loaded));
    REQUIRE(loaded.size() == 50000);
    REQUIRE(loaded.at(49999).rawTitle == "Track 49999");
}

TEST_CASE("SHA-256 matches the published vectors", "[library]") {
    REQUIRE(sha256Hex({}) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(sha256Hex(bytesOf("abc")) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(sha256Hex(bytesOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // The padding is where a hand-carried SHA-256 goes wrong. 55/56 straddle the
    // point where the 64-bit length no longer fits beside the message in one
    // block; 63/64 straddle an exact block; 119/120 repeat both a block later.
    REQUIRE(sha256Hex(bytesOf(std::string(55, 'a'))) ==
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    REQUIRE(sha256Hex(bytesOf(std::string(56, 'a'))) ==
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    REQUIRE(sha256Hex(bytesOf(std::string(63, 'a'))) ==
            "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    REQUIRE(sha256Hex(bytesOf(std::string(64, 'a'))) ==
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    REQUIRE(sha256Hex(bytesOf(std::string(119, 'a'))) ==
            "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    REQUIRE(sha256Hex(bytesOf(std::string(120, 'a'))) ==
            "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}
