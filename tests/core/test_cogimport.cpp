// Reading a Cog library.
//
// Against a synthetic store, built by tools/cogimport-fixture/make-fixture.py,
// whose rows exist one per case the reader has to get right -- see
// docs/COGIMPORT.md. A real store is someone's listening history and belongs in
// a repository even less than it would fit in one.
//
// The two cases worth naming here, because they are the ones a reader passes by
// accident on a tidier store: that ZINDEX rather than Z_PK orders the playlist,
// and that Cog prunes entries on load, so a reader that keeps them produces a
// playlist Cog itself would not show.

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/CogImport.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace xpcog;
using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path fixtureStore() {
    return std::filesystem::path{XPCOG_COG_FIXTURE_DIR} / "DataModel.sqlite";
}

[[nodiscard]] const CogEntry* entryEnding(const CogLibrary& library,
                                          std::string_view suffix) {
    const auto it = std::find_if(
        library.entries.begin(), library.entries.end(), [suffix](const CogEntry& e) {
            const std::string text = e.url.toString();
            return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        });
    return it == library.entries.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("a Cog store reads as a playlist", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    // Fifteen rows, three of which Cog itself would prune.
    CHECK(library->prunedDeleted == 1);
    CHECK(library->prunedEmptyUrl == 2);
    CHECK(library->entries.size() == 12);

    // Nothing pruned is reachable.
    CHECK(entryEnding(*library, "gone.flac") == nullptr);
}

TEST_CASE("the playlist order is ZINDEX, not the natural order", "[cogimport]") {
    // The fixture has two entries whose ZINDEX order disagrees with their Z_PK
    // order, which is the whole reason they are in it: on a store where the two
    // agree -- a playlist built once and never reordered -- a reader that
    // forgets to sort is indistinguishable from one that does not.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    // Dense and ascending, which is what "in playlist order" has to mean.
    for (std::size_t i = 1; i < library->entries.size(); ++i) {
        INFO("entry " << i);
        CHECK(library->entries[i].index > library->entries[i - 1].index);
    }

    // And specifically the pair: Sixth is ZINDEX 13 on Z_PK 15, Seventh is
    // ZINDEX 14 on Z_PK 14, so insertion order would put them the other way up.
    const CogEntry* sixth   = entryEnding(*library, "06%20Sixth.flac");
    const CogEntry* seventh = entryEnding(*library, "07%20Seventh.flac");
    REQUIRE(sixth != nullptr);
    REQUIRE(seventh != nullptr);
    CHECK(sixth->index < seventh->index);
}

TEST_CASE("a cue sheet track keeps its fragment", "[cogimport]") {
    // The happiest thing about this format: Cog stores what xpcog::Url already
    // models, fragment convention included. A cue track transfers as one URL
    // rather than as a path plus a track index, so nothing here has to invent a
    // representation and then agree with itself about it.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const CogEntry* first = entryEnding(*library, "Album.cue#01");
    REQUIRE(first != nullptr);
    CHECK(first->url.fragment() == "01");
    CHECK(first->url.extension() == "cue");

    // Two tracks, one file, and they must not collapse into each other.
    const CogEntry* second = entryEnding(*library, "Album.cue#02");
    REQUIRE(second != nullptr);
    CHECK(second->url.withoutFragment() == first->url.withoutFragment());
    CHECK(second->url != first->url);
}

TEST_CASE("a non-ASCII path survives percent-decoding", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const CogEntry* bjork = entryEnding(*library, "02%20Hyperballad.flac");
    REQUIRE(bjork != nullptr);

    // Stored percent-encoded, as a real store holds it, and the path comes back
    // as the bytes that name the file rather than as the escapes.
    //
    // Compared through pathToUtf8() rather than path::string(), which is the
    // project's own rule and one this test broke on its first run: on Windows
    // `.string()` narrows the native wide path to the *active code page*, so
    // UTF-8 bytes are never found in it however right the path is. FilePath.hpp
    // opens by naming exactly that ("wide -> CP-1252, lossy"), and Url.hpp
    // records the bug it caused -- a folder named "Björk - Post" reaching the
    // scanner as "BjÃ¶rk - Post". The path here was correct on all four
    // platforms; only the assertion was not.
    const auto path = bjork->url.localPath();
    REQUIRE(path.has_value());
    CHECK(pathToUtf8(*path).find("Bj\xC3\xB6rk") != std::string::npos);
}

TEST_CASE("a file reference URL is flagged rather than dropped or trusted",
          "[cogimport]") {
    // file:///.file/id=… names a file by inode. It is a real entry pointing at a
    // real file, so dropping it loses a track; it is also not a path, so passing
    // it on as one produces a track that cannot be opened. The third answer is
    // to carry it and say what it is, and leave resolving it to a caller that
    // has Foundation.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());
    CHECK(library->fileReferences == 1);

    const auto it = std::find_if(library->entries.begin(), library->entries.end(),
                                 [](const CogEntry& e) { return e.fileReference; });
    REQUIRE(it != library->entries.end());
    CHECK(it->url.toString() == "file:///.file/id=6571714.6571725");

    CHECK(isCogFileReferenceUrl("file:///.file/id=1.2"));
    CHECK(isCogFileReferenceUrl("/.file/id=1.2"));
    CHECK_FALSE(isCogFileReferenceUrl("file:///music/a.flac"));
    CHECK_FALSE(isCogFileReferenceUrl(""));
}

TEST_CASE("a playlist is not only local files", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const CogEntry* stream = entryEnding(*library, "stream.ogg");
    REQUIRE(stream != nullptr);
    CHECK(stream->url.scheme() == "http");
    CHECK_FALSE(stream->url.localPath().has_value());
}

TEST_CASE("the session state comes across", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const auto current = std::find_if(library->entries.begin(), library->entries.end(),
                                      [](const CogEntry& e) { return e.current; });
    REQUIRE(current != library->entries.end());
    CHECK(current->currentPosition == Approx(42.5));

    const auto queued = std::find_if(library->entries.begin(), library->entries.end(),
                                     [](const CogEntry& e) { return e.queued; });
    REQUIRE(queued != library->entries.end());
    CHECK(queued->queuePosition == 0);

    // -1 is Cog's sentinel for "not queued", and it has to survive rather than
    // being normalised to 0, which is a real queue position.
    CHECK(current->queuePosition == -1);
    CHECK_FALSE(current->queued);
}

TEST_CASE("ReplayGain is taken from the store rather than recomputed",
          "[cogimport]") {
    // Worth carrying: it is exactly what a rescan would find again, and finding
    // it is the expensive part of a scan.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const CogEntry* fifth = entryEnding(*library, "05%20Fifth.flac");
    REQUIRE(fifth != nullptr);
    CHECK(fifth->replayGainTrackGain == Approx(-6.5));
    CHECK(fifth->replayGainTrackPeak == Approx(0.98));
    CHECK(fifth->replayGainAlbumGain == Approx(-5.25));
    CHECK(fifth->replayGainAlbumPeak == Approx(1.01));
}

TEST_CASE("the sandbox paths are readable, and the bookmarks are not read",
          "[cogimport]") {
    // What a non-sandboxed player wants from ZSANDBOXTOKEN is the list of places
    // to ask for access to. The bookmark blob beside each path is opaque and
    // only macOS resolves it, so this reads the half that is portable.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    REQUIRE(library->sandboxPaths.size() == 2);
    CHECK(std::find(library->sandboxPaths.begin(), library->sandboxPaths.end(),
                    "/music") != library->sandboxPaths.end());
}

TEST_CASE("a play count matches on every field Cog recorded", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());
    const CogPlayCounts& counts = library->playCounts;
    REQUIRE(counts.size() == 2);

    // The full tuple, all four fields present and agreeing.
    const CogPlayCount* first =
        counts.find("01 First.flac", "First", "Artist", "Album");
    REQUIRE(first != nullptr);
    CHECK(first->count == 7);
    CHECK(first->rating == Approx(4.5));

    // A field that disagrees is a different track, even when the filename is
    // the one that would have matched. This is the collision the full tuple was
    // chosen to avoid: the same "01 First.flac" under a different album.
    CHECK(counts.find("01 First.flac", "First", "Artist", "Other Album") == nullptr);
    CHECK(counts.find("01 First.flac", "Different", "Artist", "Album") == nullptr);
    CHECK(counts.find("nothing.flac", "First", "Artist", "Album") == nullptr);
}

TEST_CASE("a field Cog never recorded does not constrain the match",
          "[cogimport]") {
    // The half that makes the full tuple usable rather than theoretical. On the
    // store this was designed against, 78 rows in 84 carry no artist and no
    // album -- Cog simply had not filled them in. Requiring those to be equal
    // would have imported six play counts out of eighty-four, which is not a
    // stricter match, it is a broken one: an empty field cannot disagree with
    // anything.
    std::vector<CogPlayCount> rows;
    CogPlayCount              sparse;
    sparse.filename = "02 Hyperballad.flac";
    sparse.title    = "Hyperballad";
    sparse.count    = 3;
    rows.push_back(sparse);

    const CogPlayCounts counts{std::move(rows)};

    // The two fields it did record must match; the two it did not are ignored,
    // whatever the track happens to carry.
    const CogPlayCount* found =
        counts.find("02 Hyperballad.flac", "Hyperballad", "Björk", "Post");
    REQUIRE(found != nullptr);
    CHECK(found->count == 3);

    CHECK(counts.find("02 Hyperballad.flac", "Hyperballad", "", "") != nullptr);

    // But a field it *did* record still has to agree.
    CHECK(counts.find("02 Hyperballad.flac", "Something Else", "Björk", "Post") ==
          nullptr);
}

TEST_CASE("a play count keeps the fragment in its filename", "[cogimport]") {
    // ZFILENAME is the last path component *with* the fragment still on it --
    // "Album.cue#01" -- which is what keeps the tracks of one cue sheet apart.
    // Url::fileName() does not include a fragment, so a caller has to join them,
    // and this is the case that says so out loud.
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library.has_value());

    const CogEntry* track = entryEnding(*library, "Album.cue#01");
    REQUIRE(track != nullptr);
    CHECK(track->url.fileName() == "Album.cue");
    CHECK(track->url.fileName() + "#" + std::string{track->url.fragment()} ==
          "Album.cue#01");
}

TEST_CASE("Cog's settings arrive under their own names", "[cogimport]") {
    // No translation table, and there must never be one. settings.def kept Cog's
    // spellings on purpose, so the filter is "does Settings::all() have a key by
    // this name" and the two programs cannot drift apart over what eq1kHz is
    // called -- a hand-written mapping would be a second copy of an agreement
    // that already exists.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>volumeScaling</key><string>albumGainWithPeak</string>
  <key>eq1kHz</key><real>-2.5</real>
  <key>GraphicEQtrackgenre</key><true/>
  <key>synthSampleRate</key><integer>48000</integer>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    CHECK(report.applied == 4);
    CHECK(report.ignored == 0);
    CHECK(report.mismatched == 0);

    CHECK(settings.VolumeScaling() == "albumGainWithPeak");
    CHECK(settings.Eq1kHz() == Approx(-2.5));
    CHECK(settings.GraphicEqTrackGenre());
    CHECK(settings.SynthSampleRate() == 48000);
}

TEST_CASE("a key XPCog has never heard of is ignored, not guessed at",
          "[cogimport]") {
    // Most of a real defaults file by count is AppKit's own window and toolbar
    // state, and the rest of what is left is Cog-only. None of it should reach
    // the settings, and none of it is an error either. (pitch and tempo used
    // to sit in this list; they left it when the stretchers were ported, and
    // the case below pins that they now cross over instead.)
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>miniPlusMode</key><false/>
  <key>toolbarStyleFull</key><true/>
  <key>metadataMigrated</key><true/>
  <key>NSWindow Frame Cog</key><string>0 0 1200 800 0 0 1920 1080</string>
  <key>volumeScaling</key><string>trackGain</string>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    CHECK(report.applied == 1);
    CHECK(report.ignored == 4);
    CHECK(settings.VolumeScaling() == "trackGain");
}

TEST_CASE("Cog's speed setup crosses over with the stretchers ported",
          "[cogimport]") {
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>pitch</key><real>1.25</real>
  <key>tempo</key><real>0.8</real>
  <key>speedLock</key><false/>
  <key>rubberbandEngine</key><string>finer</string>
  <key>rubberbandFormant</key><string>preserved</string>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    CHECK(report.applied == 5);
    CHECK(report.ignored == 0);
    CHECK(settings.Pitch() == Approx(1.25));
    CHECK(settings.Tempo() == Approx(0.8));
    CHECK_FALSE(settings.SpeedLock());
    CHECK(settings.RubberbandEngine() == "finer");
    CHECK(settings.RubberbandFormant() == "preserved");
}

// --- turning a store into a playlist --------------------------------------

TEST_CASE("a Cog library becomes playlist entries in Cog's order",
          "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);

    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // One entry per surviving row, in the same order. The prunes have already
    // happened by this point -- this converts what the reader handed over and
    // does not second-guess it.
    REQUIRE(imported.entries.size() == library->entries.size());
    for (std::size_t i = 0; i < imported.entries.size(); ++i) {
        CHECK(imported.entries[i].url.toString() == library->entries[i].url.toString());
    }
}

TEST_CASE("an imported entry is not pretending to have been scanned",
          "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);
    REQUIRE(!imported.entries.empty());

    // The whole reason the metadata blob is left unread: what comes out here has
    // no tags, and the caller is expected to scan. An entry claiming
    // metadataLoaded would stop the scanner ever looking at the file, which is
    // how an import would quietly produce a playlist of file names.
    for (const PlaylistEntry& entry : imported.entries) {
        CHECK(entry.metadataLoaded == false);
        CHECK(entry.artist.str().empty());
        CHECK(entry.album.str().empty());
        CHECK(entry.rawTitle.empty());
    }
}

TEST_CASE("cached stream properties come across", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // Carried because a rescan may not reach the file -- a drive that is not
    // plugged in, a share that is not mounted -- and a row that can still say
    // "FLAC, 44.1 kHz" beats one that says nothing.
    CHECK(imported.withCachedProperties > 0);

    const PlaylistEntry& first = imported.entries.front();
    CHECK(first.properties.format.sampleRate == Approx(44100.0));
    CHECK(first.properties.format.channels == 2);
    CHECK(first.properties.totalFrames == 4410000);
    CHECK(std::string{first.properties.codec.str()} == "FLAC");
}

TEST_CASE("ReplayGain is taken from the store, and a bare zero is not",
          "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // Exactly one fixture row carries a real analysis.
    CHECK(imported.withReplayGain == 1);

    const auto it = std::find_if(
        imported.entries.begin(), imported.entries.end(),
        [](const PlaylistEntry& e) { return !e.properties.replayGain.empty(); });
    REQUIRE(it != imported.entries.end());

    REQUIRE(it->properties.replayGain.trackGain);
    CHECK(*it->properties.replayGain.trackGain == Approx(-6.5F));
    REQUIRE(it->properties.replayGain.trackPeak);
    CHECK(*it->properties.replayGain.trackPeak == Approx(0.98F));
    REQUIRE(it->properties.replayGain.albumGain);
    CHECK(*it->properties.replayGain.albumGain == Approx(-5.25F));

    // Every other row has none, rather than a 0 dB gain with no peak. The store
    // cannot tell "no analysis" from "0.0" in the gain alone, so the peak is
    // what decides -- a real scan always produces one above zero. Getting this
    // wrong would give every unanalysed track a ReplayGain of 0 dB: inaudible,
    // but it makes replayGain.empty() false everywhere and puts the "no
    // information" path permanently out of reach.
    const auto analysed = std::count_if(
        imported.entries.begin(), imported.entries.end(),
        [](const PlaylistEntry& e) { return !e.properties.replayGain.empty(); });
    CHECK(analysed == 1);
}

TEST_CASE("the session's own state comes across", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // Where Cog had got to. This is the half a file cannot say for itself, and
    // is most of the reason to read the store at all.
    REQUIRE(imported.currentIndex);
    CHECK(imported.entries[*imported.currentIndex].currentPosition == Approx(42.5));

    // The queue. Cog's -1 sentinel is PlaylistEntry's too, so it copies rather
    // than translates -- and a row that was not queued must stay at -1 rather
    // than becoming position 0, which would put every track in the queue.
    const auto queued = std::count_if(imported.entries.begin(), imported.entries.end(),
                                      [](const PlaylistEntry& e) { return e.queued(); });
    CHECK(queued == 1);

    const auto it = std::find_if(imported.entries.begin(), imported.entries.end(),
                                 [](const PlaylistEntry& e) { return e.queued(); });
    REQUIRE(it != imported.entries.end());
    CHECK(it->queuePosition == 0);
}

TEST_CASE("play counts land on a scanned entry", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // Stand in for the scanner, filling only the four fields Cog keys on.
    for (PlaylistEntry& entry : imported.entries) {
        if (entry.filename() == "01 First.flac") {
            entry.rawTitle       = "First";
            entry.artist         = SharedString{"Artist"};
            entry.album          = SharedString{"Album"};
            entry.metadataLoaded = true;
        }
    }

    const CogPlayCountReport report =
        applyCogPlayCounts(library->playCounts, imported.entries);

    const auto it = std::find_if(
        imported.entries.begin(), imported.entries.end(),
        [](const PlaylistEntry& e) { return e.filename() == "01 First.flac"; });
    REQUIRE(it != imported.entries.end());
    CHECK(it->playCount == 7);

    CHECK(report.matched >= 1);
    // Per entry, not per store row, so the two sum to what was passed in.
    CHECK(report.matched + report.unmatched == imported.entries.size());
}

TEST_CASE("a play count needs every field Cog recorded to agree", "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // The same row, scanned with a different artist than the one Cog stored.
    // A field Cog *did* record must match exactly -- it is only the fields it
    // left empty that do not constrain the match.
    for (PlaylistEntry& entry : imported.entries) {
        if (entry.filename() == "01 First.flac") {
            entry.rawTitle       = "First";
            entry.artist         = SharedString{"Somebody Else"};
            entry.album          = SharedString{"Album"};
            entry.metadataLoaded = true;
        }
    }

    static_cast<void>(applyCogPlayCounts(library->playCounts, imported.entries));

    const auto it = std::find_if(
        imported.entries.begin(), imported.entries.end(),
        [](const PlaylistEntry& e) { return e.filename() == "01 First.flac"; });
    REQUIRE(it != imported.entries.end());
    CHECK(it->playCount == 0);
}

// --- merging the store back onto a scan ------------------------------------

namespace {

/// A scanned entry: what the scanner would have returned for `url`.
[[nodiscard]] PlaylistEntry scannedEntry(std::string_view url) {
    PlaylistEntry entry;
    entry.url            = *Url::parse(url);
    entry.metadataLoaded = true;
    entry.rawTitle       = "Scanned Title";
    entry.artist         = SharedString{"Scanned Artist"};
    entry.properties.totalFrames       = 1000;
    entry.properties.format.sampleRate = 48000.0;
    entry.properties.format.channels   = 2;
    return entry;
}

/// A store entry carrying the things a file cannot say for itself.
[[nodiscard]] PlaylistEntry storedEntry(std::string_view url) {
    PlaylistEntry entry;
    entry.url             = *Url::parse(url);
    entry.queuePosition   = 3;
    entry.shuffleIndex    = 9;
    entry.currentPosition = 42.5;
    entry.properties.totalFrames       = 999999;
    entry.properties.format.sampleRate = 44100.0;
    entry.properties.replayGain.trackGain = -6.5F;
    entry.properties.replayGain.trackPeak = 0.98F;
    return entry;
}

}  // namespace

TEST_CASE("the scan wins on what it established", "[cogimport]") {
    const std::vector<PlaylistEntry> store{storedEntry("file:///music/a.flac")};
    std::vector<PlaylistEntry>       scanned{scannedEntry("file:///music/a.flac")};

    CHECK(mergeCogStoreData(store, scanned) == 1);

    // The scanner opened the file. Its numbers are current; the store's are a
    // cache of unknown age, and letting them win would mean an import quietly
    // replacing correct durations with stale ones.
    CHECK(scanned[0].properties.totalFrames == 1000);
    CHECK(scanned[0].properties.format.sampleRate == Approx(48000.0));
    CHECK(scanned[0].rawTitle == "Scanned Title");
}

TEST_CASE("the store fills what the scan could not find", "[cogimport]") {
    const std::vector<PlaylistEntry> store{storedEntry("file:///music/a.flac")};
    std::vector<PlaylistEntry>       scanned{scannedEntry("file:///music/a.flac")};

    // None of these is a property of the audio, so a scan can never have found
    // them and they are copied unconditionally.
    CHECK(mergeCogStoreData(store, scanned) == 1);
    CHECK(scanned[0].queuePosition == 3);
    CHECK(scanned[0].shuffleIndex == 9);
    CHECK(scanned[0].currentPosition == Approx(42.5));

    // The file carried no ReplayGain tags, so Cog's analysis is used rather than
    // nothing -- computing it again is the expensive half of a scan.
    REQUIRE(scanned[0].properties.replayGain.trackGain);
    CHECK(*scanned[0].properties.replayGain.trackGain == Approx(-6.5F));
}

TEST_CASE("a file's own ReplayGain is not overwritten by the store's",
          "[cogimport]") {
    const std::vector<PlaylistEntry> store{storedEntry("file:///music/a.flac")};
    std::vector<PlaylistEntry>       scanned{scannedEntry("file:///music/a.flac")};
    scanned[0].properties.replayGain.trackGain = -3.0F;

    CHECK(mergeCogStoreData(store, scanned) == 1);

    // The direction that matters. A file carrying its own tags is more current
    // than Cog's copy of them, and this is the assertion that fails if somebody
    // simplifies the merge into an unconditional copy.
    REQUIRE(scanned[0].properties.replayGain.trackGain);
    CHECK(*scanned[0].properties.replayGain.trackGain == Approx(-3.0F));
}

TEST_CASE("an unreachable file keeps the store's cached properties",
          "[cogimport]") {
    const std::vector<PlaylistEntry> store{storedEntry("file:///music/a.flac")};

    // What a scan returns for a file on a drive that is not plugged in: the URL
    // and nothing else.
    std::vector<PlaylistEntry> scanned;
    PlaylistEntry              missing;
    missing.url = *Url::parse("file:///music/a.flac");
    scanned.push_back(missing);

    CHECK(mergeCogStoreData(store, scanned) == 1);

    // A row that can still say "44.1 kHz, and this long" beats one that says
    // nothing at all, which is the entire reason these are carried.
    CHECK(scanned[0].properties.totalFrames == 999999);
    CHECK(scanned[0].properties.format.sampleRate == Approx(44100.0));
}

TEST_CASE("the merge is by URL, not by position", "[cogimport]") {
    // The scan dropped the first entry -- unreadable -- so the sequences are no
    // longer aligned. Merging by index would give b.flac the gain that belongs
    // to a.flac, and nothing anywhere would complain: a wrong ReplayGain is
    // still a plausible ReplayGain.
    std::vector<PlaylistEntry> store;
    store.push_back(storedEntry("file:///music/a.flac"));
    PlaylistEntry second = storedEntry("file:///music/b.flac");
    second.properties.replayGain.trackGain = -1.25F;
    second.queuePosition                   = 7;
    store.push_back(second);

    std::vector<PlaylistEntry> scanned{scannedEntry("file:///music/b.flac")};

    CHECK(mergeCogStoreData(store, scanned) == 1);
    REQUIRE(scanned[0].properties.replayGain.trackGain);
    CHECK(*scanned[0].properties.replayGain.trackGain == Approx(-1.25F));
    CHECK(scanned[0].queuePosition == 7);
}

TEST_CASE("an entry the store never held is left alone", "[cogimport]") {
    // The scan expanded a container into tracks the store has no row for. They
    // are not errors and must not be touched.
    const std::vector<PlaylistEntry> store{storedEntry("file:///music/a.flac")};
    std::vector<PlaylistEntry>       scanned{scannedEntry("file:///music/other.flac")};

    CHECK(mergeCogStoreData(store, scanned) == 0);
    CHECK(scanned[0].queuePosition == -1);
    CHECK(scanned[0].currentPosition == Approx(0.0));
    CHECK(scanned[0].properties.replayGain.empty());
}

TEST_CASE("a file reference is kept and marked rather than dropped",
          "[cogimport]") {
    const auto library = readCogLibrary(fixtureStore());
    REQUIRE(library);
    const CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    // The fixture may hold none -- a current Cog normalises them away on load --
    // so this asserts the relationship rather than a number. What must never
    // happen is a silently shorter playlist: every reference that came in is
    // still a row, and every one of those rows says why it will not play.
    CHECK(imported.fileReferences == library->fileReferences);

    const auto marked = std::count_if(imported.entries.begin(), imported.entries.end(),
                                      [](const PlaylistEntry& e) { return e.error; });
    CHECK(static_cast<std::size_t>(marked) == imported.fileReferences);
}

TEST_CASE("the scrobbling switch is refused even though the key matches",
          "[cogimport]") {
    // The one key that exists on both sides under the same name and is still
    // not copied. Everything else in this file establishes that a shared
    // spelling means a shared value; this establishes the exception, because an
    // exception nobody tested would be removed by the next person who noticed
    // the import has no translation table.
    //
    // The credential cannot follow the switch -- Cog's session key is in the
    // macOS Keychain -- so importing "on" would show scrobbling as enabled with
    // nothing behind it. And Cog registers this as @YES by default, so "on" in a
    // real plist mostly means nobody turned it off.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    constexpr std::string_view xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
    <key>enableAudioScrobbler</key><true/>
    <key>lastFmUsername</key><string>somebody</string>
    <key>volumeScaling</key><string>trackGain</string>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    // Left at XPCog's own default, which is off.
    CHECK(settings.EnableScrobbling() == false);

    // Counted as ignored rather than mismatched: nothing disagrees about the
    // type, this is a key we decline. `lastFmUsername` is ignored too, but for
    // the ordinary reason -- XPCog has no such setting, because the username
    // lives with the session key in the platform's secret store.
    CHECK(report.ignored == 2);
    CHECK(report.mismatched == 0);
    CHECK(report.applied == 1);

    // And the rest of the file still imports, so refusing one key does not
    // abandon the import.
    CHECK(settings.VolumeScaling() == "trackGain");
}

TEST_CASE("an absent key leaves the setting alone", "[cogimport]") {
    // The half that makes this an import rather than a reset. NSUserDefaults
    // persists only what differs from what was registered, so a real file holds
    // a handful of keys -- and the absence of eq1kHz means "Cog's default",
    // which is XPCog's default too. Writing defaults for everything the file did
    // not mention would replace the user's XPCog configuration with Cog's.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    settings.setEq1kHz(7.5);
    settings.setVolumeScaling("albumGain");

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>eq20Hz</key><real>3.0</real>
</dict></plist>)";

    importCogSettings(xml, settings, nullptr);

    CHECK(settings.Eq20Hz() == Approx(3.0));
    // Untouched, both of them.
    CHECK(settings.Eq1kHz() == Approx(7.5));
    CHECK(settings.VolumeScaling() == "albumGain");
}

TEST_CASE("numbers convert across their spellings, shapes do not",
          "[cogimport]") {
    // A plist writes 1 as an integer and 1.0 as a real depending on how the
    // value was set, so refusing <real>1</real> for an integer setting would
    // drop a value over its spelling. What is refused is a different kind of
    // disagreement: an array where a number was declared means the two programs
    // do not agree about what the key is, and that is worth counting rather than
    // coercing.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>synthSampleRate</key><real>44100.4</real>
  <key>eq20Hz</key><integer>4</integer>
  <key>GraphicEQtrackgenre</key><integer>1</integer>
  <key>volumeScaling</key><array><string>nonsense</string></array>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    CHECK(report.applied == 3);
    CHECK(report.mismatched == 1);

    // Rounded, not truncated: a value that made this trip repeatedly would
    // otherwise drift downward each time.
    CHECK(settings.SynthSampleRate() == 44100);
    CHECK(settings.Eq20Hz() == Approx(4.0));
    CHECK(settings.GraphicEqTrackGenre());
}

TEST_CASE("a defaults file that is not one changes nothing", "[cogimport]") {
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setEq1kHz(1.5);

    CogSettingsReport report;
    importCogSettings("not a plist at all", settings, &report);
    CHECK(report.applied == 0);

    // An array at the root parses, and is still not a defaults file.
    importCogSettings(R"(<?xml version="1.0"?><plist version="1.0"><array/></plist>)",
                      settings, &report);
    CHECK(report.applied == 0);

    CHECK(settings.Eq1kHz() == Approx(1.5));
}

TEST_CASE("a file that is not a Cog store is refused", "[cogimport]") {
    // Distinguished from an empty library, which is a valid answer: a store with
    // no rows in it is a Cog install nobody has added anything to.
    CHECK_FALSE(readCogLibrary("/nonexistent/DataModel.sqlite").has_value());
    CHECK_FALSE(
        readCogLibrary(std::filesystem::path{XPCOG_COG_FIXTURE_DIR} /
                       "org.cogx.cog.plist")
            .has_value());
}

TEST_CASE("reading a store neither creates nor modifies one", "[cogimport]") {
    // Both halves were real. sql::Database::open() passes SQLITE_OPEN_CREATE,
    // so a path naming nothing became an empty database rather than an error --
    // a read that "failed" and left a file behind. And it sets the journal mode
    // on connect, which rewrites the header of a database opened only to be
    // read.
    //
    // Neither is theoretical for this reader in particular: the file it opens
    // belongs to another program and may be the one that program is running
    // from. It was noticed because the checked-in fixture came back modified
    // after every test run.
    namespace fs = std::filesystem;

    const fs::path absent =
        fs::temp_directory_path() / "xpcog-cogimport-must-not-exist.sqlite";
    fs::remove(absent);

    CHECK_FALSE(readCogLibrary(absent).has_value());
    CHECK_FALSE(fs::exists(absent));

    // And the fixture is untouched, which is the property that makes it a
    // fixture rather than a working file.
    const fs::path store = fs::path{XPCOG_COG_FIXTURE_DIR} / "DataModel.sqlite";
    const auto     before = fs::last_write_time(store);
    const auto     size   = fs::file_size(store);

    REQUIRE(readCogLibrary(store).has_value());

    // Compared into a bool first: Catch2 cannot stream a file_time_type, and an
    // assertion that will not compile is worth less than one that reads slightly
    // worse.
    const bool timeUnchanged = fs::last_write_time(store) == before;
    CHECK(timeUnchanged);
    CHECK(fs::file_size(store) == size);
}
