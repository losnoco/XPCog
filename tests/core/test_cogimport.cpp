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
    // state, and the rest of what is left is Cog-only -- pitch and tempo are
    // Rubber Band, which is not ported. None of it should reach the settings,
    // and none of it is an error either.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>pitch</key><real>1.0</real>
  <key>tempo</key><real>1.0</real>
  <key>miniPlusMode</key><false/>
  <key>metadataMigrated</key><true/>
  <key>NSWindow Frame Cog</key><string>0 0 1200 800 0 0 1920 1080</string>
  <key>volumeScaling</key><string>trackGain</string>
</dict></plist>)";

    CogSettingsReport report;
    importCogSettings(xml, settings, &report);

    CHECK(report.applied == 1);
    CHECK(report.ignored == 5);
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
