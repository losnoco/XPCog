// Cue sheet tests.
//
// A cue sheet turns one audio file into several tracks, so the failure modes are
// boundary arithmetic: a track that starts late, runs long, or inherits the wrong
// metadata. Real sheets in the wild also use CRLF, quoted REM values and INDEX 00
// pre-gaps, so those are covered here rather than only the tidy synthetic case.

#include "../../codecs/cuesheet/CueSheet.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate     = 44100.0;
constexpr int    kSegments = 3;
constexpr int    kSegSecs  = 2;

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-cue-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

std::filesystem::path writeText(const std::string& name, const std::string& body) {
    const auto path = fixtureDir() / name;
    std::FILE* f    = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return path;
}

/// A single FLAC of three distinct tones, so each cue track has a recognisable
/// frequency and a misaligned boundary is measurable rather than merely suspected.
std::filesystem::path albumFlac() {
    static const std::filesystem::path path = [] {
        const auto wav = fixtureDir() / "album.wav";

        std::vector<std::int16_t> samples;
        for (int seg = 0; seg < kSegments; ++seg) {
            const double freq = 300.0 * (seg + 1);
            for (int i = 0; i < static_cast<int>(kRate) * kSegSecs; ++i) {
                const auto v = static_cast<std::int16_t>(
                    18000.0 * std::sin(xpcog::test::kTwoPi * freq * (i / kRate)));
                samples.push_back(v);
                samples.push_back(v);
            }
        }

        const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
        std::FILE* f         = std::fopen(wav.string().c_str(), "wb");
        const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(2);
        u32(static_cast<std::uint32_t>(kRate));
        u32(static_cast<std::uint32_t>(kRate) * 4);
        u16(4); u16(16);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        std::fwrite(samples.data(), 1, dataBytes, f);
        std::fclose(f);

        const auto flac = fixtureDir() / "album.flac";
        const std::string cmd = "flac -s -f --totally-silent -o \"" + flac.string() +
                                "\" \"" + wav.string() + "\" 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) {
            return std::filesystem::path{};
        }
        return flac;
    }();
    return path;
}

/// The sheet describing albumFlac(), written once per process.
///
/// Built here rather than inside whichever test happens to need it first.
/// catch_discover_tests runs every test case in its own process and ctest
/// orders them by name, so a test that only *reads* this file silently depends
/// on a different test having run before it -- and "a cue track reports its own
/// duration" sorts before "cue tracks decode the correct span", which was the
/// only writer. That passed everywhere it had ever run, because the file was
/// already sitting in the temp directory from an earlier run. On a clean
/// machine it failed, which is exactly what CI found the first time these tests
/// were not being skipped.
std::filesystem::path albumCue() {
    static const std::filesystem::path path = writeText(
        "album.cue",
        "PERFORMER \"Artist\"\n"
        "TITLE \"Album\"\n"
        "FILE \"album.flac\" WAVE\n"
        "  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n    TITLE \"Two\"\n    INDEX 01 00:02:00\n"
        "  TRACK 03 AUDIO\n    TITLE \"Three\"\n    INDEX 01 00:04:00\n");
    return path;
}

std::vector<float> decodeAll(const Url& url) {
    auto opened = registry().open(url);
    REQUIRE(opened);

    std::vector<float> out;
    AudioChunk         chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = out.size();
        out.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{out}.subspan(offset, samples));
    }
    return out;
}

double dominantFrequency(const std::vector<float>& pcm) {
    const std::size_t frames = pcm.size() / 2;
    const std::size_t begin  = frames / 4;
    const std::size_t end    = frames * 3 / 4;

    std::size_t crossings = 0;
    float       previous  = 0.0F;
    for (std::size_t i = begin; i < end; ++i) {
        const float v = pcm[i * 2];
        if (i > begin && ((previous < 0.0F) != (v < 0.0F))) {
            ++crossings;
        }
        previous = v;
    }
    return static_cast<double>(crossings) * kRate / (2.0 * static_cast<double>(end - begin));
}

const bool kHaveCue = registry().isContainer(Url::fromLocalPath("x.cue"));

}  // namespace

// --- parser ---------------------------------------------------------------

TEST_CASE("cue parser reads tracks, times and metadata", "[cue]") {
    const std::string sheet =
        "REM GENRE \"Progressive Rock\"\r\n"
        "REM DATE 1977\r\n"
        "PERFORMER \"Album Artist\"\r\n"
        "TITLE \"The Album\"\r\n"
        "FILE \"album.flac\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE \"One\"\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"Two\"\r\n"
        "    PERFORMER \"Guest\"\r\n"
        "    INDEX 00 00:01:50\r\n"
        "    INDEX 01 00:02:00\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    TITLE \"Three\"\r\n"
        "    INDEX 01 00:04:00\r\n";

    const auto parsed = codecs::CueSheet::parse(
        sheet, Url::fromLocalPath(fixtureDir() / "x.cue"));

    REQUIRE(parsed.tracks().size() == 3);

    // INDEX 01 starts the track; INDEX 00 is pre-gap and must be ignored.
    CHECK(parsed.tracks()[1].time == Catch::Approx(2.0));

    // Quoted REM values keep their spaces.
    CHECK(parsed.tracks()[0].genre == "Progressive Rock");
    CHECK(parsed.tracks()[0].year == "1977");
    CHECK(parsed.tracks()[0].album == "The Album");

    // A track-level PERFORMER applies to that track only. Cog leaks it into every
    // following track; here track 3 falls back to the album artist.
    CHECK(parsed.tracks()[0].artist == "Album Artist");
    CHECK(parsed.tracks()[1].artist == "Guest");
    CHECK(parsed.tracks()[2].artist == "Album Artist");

    // Likewise, titles do not bleed forward.
    CHECK(parsed.tracks()[0].title == "One");
    CHECK(parsed.tracks()[2].title == "Three");
}

TEST_CASE("cue parser converts mm:ss:ff using a 75 Hz frame clock", "[cue]") {
    const std::string sheet =
        "FILE \"a.flac\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 01:24:40\n";

    const auto parsed =
        codecs::CueSheet::parse(sheet, Url::fromLocalPath(fixtureDir() / "x.cue"));
    REQUIRE(parsed.tracks().size() == 1);

    // 1 min 24 sec 40 frames = 84 + 40/75 seconds.
    CHECK(parsed.tracks()[0].time == Catch::Approx(84.0 + 40.0 / 75.0));
    CHECK_FALSE(parsed.tracks()[0].timeInSamples);
    CHECK(parsed.tracks()[0].startFrame(kRate) ==
          static_cast<std::int64_t>((84.0 + 40.0 / 75.0) * kRate));
}

TEST_CASE("cue parser skips non-audio tracks", "[cue]") {
    const std::string sheet =
        "FILE \"a.bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:10:00\n";

    const auto parsed =
        codecs::CueSheet::parse(sheet, Url::fromLocalPath(fixtureDir() / "x.cue"));
    REQUIRE(parsed.tracks().size() == 1);
    CHECK(parsed.tracks()[0].track == "02");
}

TEST_CASE("cue track lookup tolerates zero padding", "[cue]") {
    const std::string sheet =
        "FILE \"a.flac\" WAVE\n  TRACK 03 AUDIO\n    INDEX 01 00:00:00\n";
    const auto parsed =
        codecs::CueSheet::parse(sheet, Url::fromLocalPath(fixtureDir() / "x.cue"));

    REQUIRE(parsed.findTrack("03") != nullptr);
    CHECK(parsed.findTrack("3") != nullptr);   // unpadded also resolves
    CHECK(parsed.findTrack("4") == nullptr);
}

// --- decoding -------------------------------------------------------------

TEST_CASE("cue tracks decode the correct span of the audio file", "[cue]") {
    if (!kHaveCue) SKIP("cue sheet support not built");
    const auto flac = albumFlac();
    if (flac.empty()) SKIP("the `flac` command-line tool is not available");

    const Url cue     = Url::fromLocalPath(albumCue());
    const auto entries = registry().expandContainer(cue);
    REQUIRE(entries.size() == 3);

    const std::vector<float> whole = decodeAll(Url::fromLocalPath(flac));

    std::size_t total = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        INFO("track " << (i + 1));
        const std::vector<float> track = decodeAll(entries[i]);

        // Each segment is a different tone, so a boundary that is off shows up
        // as the wrong frequency rather than as a subtle offset.
        CHECK(dominantFrequency(track) ==
              Catch::Approx(300.0 * static_cast<double>(i + 1)).margin(12.0));

        // And the samples must be exactly the right slice of the source file.
        REQUIRE(total + track.size() <= whole.size());
        for (std::size_t k = 0; k < track.size(); ++k) {
            if (track[k] != whole[total + k]) {
                FAIL("track " << (i + 1) << " sample " << k << " differs");
            }
        }
        total += track.size();
    }

    // The tracks must tile the file with no gap and no overlap.
    CHECK(total == whole.size());
}

TEST_CASE("a cue track reports its own duration, not the file's", "[cue]") {
    if (!kHaveCue) SKIP("cue sheet support not built");
    if (albumFlac().empty()) SKIP("the `flac` command-line tool is not available");

    const Url cue = Url::fromLocalPath(albumCue());
    auto      opened = registry().open(cue.withFragment("02"));
    REQUIRE(opened);

    const auto props = opened.decoder->properties();
    CHECK(props.duration() == Catch::Approx(static_cast<double>(kSegSecs)).margin(0.01));
    CHECK(props.seekable);
    CHECK(opened.decoder->metadata().first("title") == "Two");
}

TEST_CASE("seeking within a cue track stays inside the track", "[cue]") {
    if (!kHaveCue) SKIP("cue sheet support not built");
    if (albumFlac().empty()) SKIP("the `flac` command-line tool is not available");

    const Url cue    = Url::fromLocalPath(albumCue());
    auto      opened = registry().open(cue.withFragment("02"));
    REQUIRE(opened);

    // Seek positions are relative to the track, so 0 must be the track's start
    // rather than the file's.
    REQUIRE(opened.decoder->seek(0) == 0);

    AudioChunk chunk;
    REQUIRE(opened.decoder->readAudio(chunk));
    std::vector<float> pcm(float32SampleCount(chunk));
    convertToFloat32(chunk, pcm);

    std::vector<float> gathered = pcm;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t offset = gathered.size();
        gathered.resize(offset + float32SampleCount(chunk));
        convertToFloat32(chunk, std::span<float>{gathered}.subspan(
                                    offset, float32SampleCount(chunk)));
    }

    // Still track 2's tone, and still track 2's length.
    CHECK(dominantFrequency(gathered) == Catch::Approx(600.0).margin(12.0));
    CHECK(gathered.size() / 2 ==
          static_cast<std::size_t>(kRate) * static_cast<std::size_t>(kSegSecs));
}

TEST_CASE("a cue URL without a fragment does not decode", "[cue]") {
    if (!kHaveCue) SKIP("cue sheet support not built");
    if (albumFlac().empty()) SKIP("the `flac` command-line tool is not available");

    // Nothing identifies which track to play, so this must fail rather than
    // silently decode the whole album as one track.
    const Url cue = Url::fromLocalPath(albumCue());
    CHECK_FALSE(registry().open(cue));
}
