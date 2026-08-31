// Scanner tests: turning a folder into a playlist.
//
// Some cases need a real encoded file with real tags. Those generate one with
// `flac` and skip if it is not installed, matching the conformance harness --
// the suite still runs on a machine with no encoders.

#include "../TestShell.hpp"

#include "xpcog/core/NaturalOrder.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace xpcog;

namespace {

namespace fs = std::filesystem;

/// A directory removed when the test ends, however it ends.
class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(fs::temp_directory_path() / ("xpcog-scan-" + name)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }
    [[nodiscard]] fs::path        file(const std::string& name) const {
        return path_ / name;
    }

    void write(const std::string& name, std::string_view text) const {
        const fs::path target = path_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream out{target, std::ios::binary};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

private:
    fs::path path_;
};

using xpcog::test::haveTool;

const PluginRegistry& codecRegistry() {
    // Built once: freeze() is one-shot, and the descriptors it sorts must
    // outlive every decoder handed out from them.
    static const PluginRegistry& registry = *[] {
        auto* built = new PluginRegistry;
        registerAllCodecs(*built);
        return built;
    }();
    return registry;
}

/// A one-second FLAC with the given Vorbis comments. Returns false when `flac`
/// is not installed.
bool makeTaggedFlac(const fs::path& target, const std::vector<std::string>& tags) {
    if (!haveTool("flac")) {
        return false;
    }

    const fs::path wav = target.parent_path() / "source.wav";
    {
        // 16-bit stereo 44.1 kHz, one second of silence is enough: the test is
        // about tags, not audio.
        constexpr int kFrames = 44100;
        std::ofstream out{wav, std::ios::binary};
        const auto    write32 = [&out](std::uint32_t value) {
            const char bytes[4] = {static_cast<char>(value & 0xFF),
                                   static_cast<char>((value >> 8) & 0xFF),
                                   static_cast<char>((value >> 16) & 0xFF),
                                   static_cast<char>((value >> 24) & 0xFF)};
            out.write(bytes, 4);
        };
        const auto write16 = [&out](std::uint16_t value) {
            const char bytes[2] = {static_cast<char>(value & 0xFF),
                                   static_cast<char>((value >> 8) & 0xFF)};
            out.write(bytes, 2);
        };
        const std::uint32_t dataBytes = kFrames * 4;
        out.write("RIFF", 4);
        write32(36 + dataBytes);
        out.write("WAVEfmt ", 8);
        write32(16);
        write16(1);
        write16(2);
        write32(44100);
        write32(44100 * 4);
        write16(4);
        write16(16);
        out.write("data", 4);
        write32(dataBytes);
        const std::vector<char> silence(dataBytes, 0);
        out.write(silence.data(), dataBytes);
    }

    std::string command = "flac -s -f --totally-silent";
    for (const std::string& tag : tags) {
        command += " --tag=\"" + tag + "\"";
    }
    command += " -o \"" + target.string() + "\" \"" + wav.string() + "\"";
    command += xpcog::test::kSilenceStderr;

    const bool ok = std::system(command.c_str()) == 0;
    std::error_code error;
    fs::remove(wav, error);
    return ok && fs::exists(target);
}

}  // namespace

TEST_CASE("natural order puts track 9 before track 10", "[scanner]") {
    REQUIRE(naturalLess("track 9.flac", "track 10.flac"));
    REQUIRE_FALSE(naturalLess("track 10.flac", "track 9.flac"));

    REQUIRE(naturalLess("02 Dogs.flac", "10 Sheep.flac"));
    REQUIRE(naturalLess("2 Dogs.flac", "02 Dogs.flac") ==
            naturalLess("2 Dogs.flac", "02 Dogs.flac"));  // total, whichever way

    // Leading zeros do not change the value.
    REQUIRE_FALSE(naturalLess("007", "7"));
    REQUIRE_FALSE(naturalLess("7", "007"));

    // Numbers too long for an integer still compare correctly.
    REQUIRE(naturalLess("999999999999999999999998", "999999999999999999999999"));

    // Case decides only when nothing else does, and then by byte order, so the
    // ordering stays total: "Album" and "album" cannot compare equal.
    REQUIRE(naturalLess("Album", "album"));
    REQUIRE_FALSE(naturalLess("album", "Album"));

    // ...and case never outranks the letters themselves.
    REQUIRE(naturalLess("a", "B"));
    REQUIRE_FALSE(naturalLess("B", "a"));

    REQUIRE(naturalLess("dog", "dogs"));  // a prefix sorts first
    REQUIRE_FALSE(naturalLess("dog", "dog"));
}

TEST_CASE("a scan sorts a folder into track order", "[scanner]") {
    const TempDir dir{"order"};
    // Written out of order, and with a two-digit track that a byte sort puts
    // second.
    for (const char* name : {"10 Ten.flac", "2 Two.flac", "1 One.flac"}) {
        dir.write(name, "not really a flac");
    }
    dir.write("cover.jpg", "not audio");

    const Scanner scanner{codecRegistry()};
    const Url     root  = Url::fromLocalPath(dir.path());
    const auto    found = scanner.expand({&root, 1});

    REQUIRE(found.size() == 3);  // cover.jpg is not a claimed extension
    REQUIRE(found[0].localPath()->filename() == "1 One.flac");
    REQUIRE(found[1].localPath()->filename() == "2 Two.flac");
    REQUIRE(found[2].localPath()->filename() == "10 Ten.flac");
}

TEST_CASE("a scan expands playlists into their tracks", "[scanner]") {
    const TempDir dir{"expand"};
    dir.write("a.flac", "x");
    dir.write("b.flac", "x");
    dir.write("list.m3u", "a.flac\nb.flac\n");

    Scanner::Options options;
    options.readPlaylists = true;
    const Scanner scanner{codecRegistry(), options};

    const Url  playlist = Url::fromLocalPath(dir.file("list.m3u"));
    const auto found    = scanner.expand({&playlist, 1});

    REQUIRE(found.size() == 2);
    REQUIRE(found[0].localPath()->filename() == "a.flac");
}

TEST_CASE("duplicates collapse to the first occurrence", "[scanner]") {
    const TempDir dir{"dupes"};
    dir.write("a.flac", "x");
    dir.write("b.flac", "x");
    // The folder scan finds a.flac and b.flac; the playlist names a.flac again.
    dir.write("list.m3u", "b.flac\na.flac\n");

    const Scanner scanner{codecRegistry()};
    const Url     root  = Url::fromLocalPath(dir.path());
    const auto    found = scanner.expand({&root, 1});

    REQUIRE(found.size() == 2);
    REQUIRE(found[0].localPath()->filename() == "a.flac");
    REQUIRE(found[1].localPath()->filename() == "b.flac");
}

TEST_CASE("a playlist naming itself terminates", "[scanner]") {
    const TempDir dir{"cycle"};
    dir.write("a.flac", "x");
    dir.write("loop.m3u", "loop.m3u\na.flac\n");

    const Scanner scanner{codecRegistry()};
    const Url     playlist = Url::fromLocalPath(dir.file("loop.m3u"));
    const auto    found    = scanner.expand({&playlist, 1});

    REQUIRE(found.size() == 1);
    REQUIRE(found[0].localPath()->filename() == "a.flac");
}

TEST_CASE("a cue sheet expands to its tracks, not to nothing", "[scanner]") {
    const TempDir dir{"cue"};
    dir.write("album.flac", "x");
    dir.write("album.cue",
              "FILE \"album.flac\" WAVE\n"
              "  TRACK 01 AUDIO\n"
              "    TITLE \"One\"\n"
              "    INDEX 01 00:00:00\n"
              "  TRACK 02 AUDIO\n"
              "    TITLE \"Two\"\n"
              "    INDEX 01 01:00:00\n");

    const Scanner scanner{codecRegistry()};
    const Url     cue   = Url::fromLocalPath(dir.file("album.cue"));
    const auto    found = scanner.expand({&cue, 1});

    // A cue *track* URL keeps the .cue extension, so an expansion that does not
    // recognise a fragment as "already inside a container" re-expands each track
    // into the whole sheet, finds every entry already visited, and returns
    // nothing at all.
    REQUIRE(found.size() == 2);
    // The fragment is the TRACK number as written in the sheet, zero padding
    // included, because that is what the cue decoder matches on.
    REQUIRE(found[0].fragment() == "01");
    REQUIRE(found[1].fragment() == "02");
}

TEST_CASE("cue sheets can be left out of a folder scan", "[scanner]") {
    const TempDir dir{"cueopt"};
    dir.write("album.flac", "x");
    dir.write("album.cue", "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

    Scanner::Options options;
    options.readCueSheets = false;
    const Scanner scanner{codecRegistry(), options};

    const Url  root  = Url::fromLocalPath(dir.path());
    const auto found = scanner.expand({&root, 1});

    // Without this switch the folder yields every track twice: once from the
    // cue sheet and once from the FLAC it points at.
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].localPath()->filename() == "album.flac");
}

TEST_CASE("AppleDouble sidecars stay out of a folder scan", "[scanner]") {
    const TempDir dir{"appledouble"};
    dir.write("album.flac", "x");
    // What a Mac leaves next to the audio on a volume with no resource forks.
    // The extension is the track's, so nothing that looks at the name alone
    // tells the two apart.
    dir.write("._album.flac", "AppleDouble header, not audio");
    // A leading dot on its own is somebody's hidden file, and playable.
    dir.write(".hidden.flac", "x");

    const Url root = Url::fromLocalPath(dir.path());

    SECTION("skipped by default") {
        const Scanner scanner{codecRegistry()};
        const auto    found = scanner.expand({&root, 1});

        REQUIRE(found.size() == 2);
        REQUIRE(std::none_of(found.begin(), found.end(), [](const Url& url) {
            return url.localPath()->filename().string().starts_with("._");
        }));
    }

    SECTION("kept when the option is off") {
        Scanner::Options options;
        options.skipAppleDoubleFiles = false;
        const Scanner scanner{codecRegistry(), options};
        const auto    found = scanner.expand({&root, 1});

        REQUIRE(found.size() == 3);
    }

    SECTION("a sidecar named outright is still opened") {
        // Only folder walks are filtered. Asked for by name, the file comes
        // back -- and fails later with a reason, rather than vanishing.
        const Scanner scanner{codecRegistry()};
        const Url     sidecar = Url::fromLocalPath(dir.file("._album.flac"));
        const auto    found   = scanner.expand({&sidecar, 1});

        REQUIRE(found.size() == 1);
    }
}

TEST_CASE("an unopenable file becomes a visible error, not a gap", "[scanner]") {
    const TempDir dir{"broken"};
    dir.write("broken.flac", "this is not a FLAC file");

    const Scanner scanner{codecRegistry()};
    const Url     root = Url::fromLocalPath(dir.path());
    const auto    entries = scanner.scan({&root, 1});

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].error);
    REQUIRE_FALSE(entries[0].errorMessage.empty());
    // A file that cannot be read still has a name to show.
    REQUIRE(entries[0].title() == "broken.flac");
}

TEST_CASE("progress is reported once per entry", "[scanner]") {
    const TempDir dir{"progress"};
    dir.write("a.flac", "x");
    dir.write("b.flac", "x");

    Scanner scanner{codecRegistry()};

    std::vector<std::pair<std::size_t, std::size_t>> reports;
    scanner.setProgressCallback(
        [&reports](std::size_t done, std::size_t total) { reports.emplace_back(done, total); });

    const Url root = Url::fromLocalPath(dir.path());
    static_cast<void>(scanner.scan({&root, 1}));

    REQUIRE(reports.size() == 2);
    REQUIRE(reports[1] == std::pair<std::size_t, std::size_t>{2, 2});
}

TEST_CASE("cancelling stops a scan without failing it", "[scanner]") {
    const TempDir dir{"cancel"};
    for (int i = 0; i < 6; ++i) {
        dir.write(std::to_string(i) + ".flac", "x");
    }

    Scanner scanner{codecRegistry()};
    scanner.setProgressCallback([&scanner](std::size_t done, std::size_t) {
        if (done == 2) {
            scanner.cancel();
        }
    });

    const Url  root    = Url::fromLocalPath(dir.path());
    const auto entries = scanner.scan({&root, 1});

    REQUIRE(entries.size() == 2);
    REQUIRE(scanner.cancelled());
}

TEST_CASE("ReplayGain tags are promoted out of the tag list", "[scanner]") {
    MetadataMap tags;
    tags.set("replaygain_track_gain", "-3.50 dB");
    tags.set("replaygain_track_peak", "0.987654");
    tags.set("replaygain_album_gain", "+2.25 dB");
    tags.set("cuesheet", "REM a sheet");
    tags.set("artist", "Pink Floyd");

    TrackProperties properties;
    promoteReplayGain(tags, properties);

    REQUIRE(properties.replayGain.trackGain == -3.5F);
    REQUIRE(properties.replayGain.trackPeak == 0.987654F);
    REQUIRE(properties.replayGain.albumGain == 2.25F);
    REQUIRE_FALSE(properties.replayGain.albumPeak.has_value());
    REQUIRE(properties.cuesheet == "REM a sheet");

    // Promoted keys leave the map; everything else stays.
    REQUIRE_FALSE(tags.contains("replaygain_track_gain"));
    REQUIRE_FALSE(tags.contains("cuesheet"));
    REQUIRE(tags.first("artist") == "Pink Floyd");
}

TEST_CASE("tags are read off a real file", "[scanner]") {
    const TempDir  dir{"tags"};
    const fs::path target = dir.file("tagged.flac");

    if (!makeTaggedFlac(target, {"ARTIST=Pink Floyd", "ALBUM=Animals", "TITLE=Dogs",
                                 "TRACKNUMBER=2", "DATE=1977-01-23",
                                 "ALBUMARTIST=Pink Floyd", "MOOD=bleak",
                                 "REPLAYGAIN_TRACK_GAIN=-3.50 dB"})) {
        SKIP("flac is not installed");
    }

    const Scanner scanner{codecRegistry()};

    PlaylistEntry entry;
    entry.url = Url::fromLocalPath(target);
    REQUIRE(scanner.readMetadata(entry));

    REQUIRE_FALSE(entry.error);
    REQUIRE(entry.artist == "Pink Floyd");
    REQUIRE(entry.album == "Animals");
    REQUIRE(entry.rawTitle == "Dogs");
    REQUIRE(entry.albumArtist == "Pink Floyd");
    REQUIRE(entry.track == 2);
    REQUIRE(entry.year == 1977);
    REQUIRE(entry.display() == "Pink Floyd - Dogs");

    // A tag with no column of its own is still kept.
    REQUIRE(entry.metadata.first("mood") == "bleak");

    // ...and one that belongs in the properties has moved there.
    REQUIRE(entry.properties.replayGain.trackGain == -3.5F);
    REQUIRE_FALSE(entry.metadata.contains("replaygain_track_gain"));

    // The decoder filled in the stream properties alongside.
    REQUIRE(entry.properties.format.sampleRate == 44100.0);
    REQUIRE(entry.properties.format.channels == 2);
    REQUIRE(entry.properties.lossless);
    REQUIRE(entry.duration() == 1.0);
}

TEST_CASE("the cache spares a second read of an unchanged file", "[scanner]") {
    const TempDir  dir{"cache"};
    const fs::path target = dir.file("tagged.flac");
    if (!makeTaggedFlac(target, {"ARTIST=Pink Floyd", "TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    PluginCache cache;
    Scanner     scanner{codecRegistry()};
    scanner.setCache(&cache);

    PlaylistEntry first;
    first.url = Url::fromLocalPath(target);
    REQUIRE(scanner.readMetadata(first));
    REQUIRE(cache.statistics().hits == 0);
    REQUIRE(cache.statistics().misses == 1);

    PlaylistEntry second;
    second.url = Url::fromLocalPath(target);
    REQUIRE(scanner.readMetadata(second));
    REQUIRE(cache.statistics().hits == 1);
    REQUIRE(second.artist == "Pink Floyd");
    REQUIRE(second.rawTitle == "Dogs");
    REQUIRE(second.properties.format.sampleRate == 44100.0);
}

TEST_CASE("retagging invalidates the cache", "[scanner]") {
    const TempDir  dir{"retag"};
    const fs::path target = dir.file("tagged.flac");
    if (!makeTaggedFlac(target, {"ARTIST=Pink Floyd", "TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    PluginCache cache;
    Scanner     scanner{codecRegistry()};
    scanner.setCache(&cache);

    PlaylistEntry before;
    before.url = Url::fromLocalPath(target);
    REQUIRE(scanner.readMetadata(before));
    REQUIRE(before.rawTitle == "Dogs");

    // Rewrite the file with different tags. Cog keys its cache on the URL alone,
    // so it would keep handing back "Dogs" for the rest of the session.
    REQUIRE(makeTaggedFlac(target, {"ARTIST=Pink Floyd", "TITLE=Sheep",
                                    "COMMENT=padding to change the size"}));

    PlaylistEntry after;
    after.url = Url::fromLocalPath(target);
    REQUIRE(scanner.readMetadata(after));
    REQUIRE(after.rawTitle == "Sheep");
}

TEST_CASE("a cached entry does not carry another row's identity", "[scanner]") {
    PluginCache cache;

    PlaylistEntry stored;
    stored.url           = Url::fromLocalPath("/music/dogs.flac");
    stored.id            = 42;
    stored.queuePosition = 3;
    stored.shuffleIndex  = 7;
    stored.rawTitle      = "Dogs";

    // A stamp the lookup will match; the file need not exist for this.
    const PluginCache::Stamp stamp{1234, 5678};
    cache.store(stored.url, stamp, stored);

    const auto cached = cache.lookup(stored.url, stamp);
    REQUIRE(cached.has_value());
    REQUIRE(cached->rawTitle == "Dogs");
    REQUIRE(cached->id == kInvalidTrackId);
    REQUIRE(cached->queuePosition == -1);
    REQUIRE(cached->shuffleIndex == -1);

    // A different stamp for the same URL is a different file.
    REQUIRE_FALSE(cache.lookup(stored.url, PluginCache::Stamp{1234, 9999}).has_value());
}

TEST_CASE("remote URLs are not cached", "[scanner]") {
    PluginCache cache;

    const Url stream = *Url::parse("http://example.org/stream.ogg");
    // stampFor cannot stat a URL with no local path, so it returns a zero stamp
    // -- which must never be a usable key, or every remote stream would collide
    // with every other one.
    REQUIRE(PluginCache::stampFor(stream) == PluginCache::Stamp{});

    PlaylistEntry entry;
    entry.url = stream;
    cache.store(stream, PluginCache::stampFor(stream), entry);
    REQUIRE(cache.size() == 0);
    REQUIRE_FALSE(cache.lookup(stream, PluginCache::stampFor(stream)).has_value());
}

TEST_CASE("embedded artwork moves into the library", "[scanner]") {
    Library library;
    REQUIRE(library.open(":memory:"));

    std::vector<std::byte> image;
    for (int i = 0; i < 64; ++i) {
        image.push_back(static_cast<std::byte>(i));
    }

    PlaylistEntry entry;
    entry.url = Url::fromLocalPath("/music/dogs.flac");
    entry.metadata.setBytes("albumart", image);

    REQUIRE(library.adoptArtwork(entry));
    REQUIRE(entry.artHash.size() == 64);
    REQUIRE_FALSE(entry.metadata.contains("albumart"));
    REQUIRE(library.artwork(entry.artHash) == image);

    // Nothing to move the second time.
    REQUIRE_FALSE(library.adoptArtwork(entry));
}
