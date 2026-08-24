// FLAC bit depths that are not a multiple of eight.
//
// libFLAC hands the decoder a 32-bit integer per sample carrying the value in
// its N least significant bits, sign-extended. A 12-bit sample therefore spans
// about +/-2^11, not +/-2^15, and writing it into a 16-bit container unchanged
// is a recording sixteen times too quiet -- everything downstream scales by the
// container and has no reason to ask what the source depth was.
//
// The fixtures are encoded here rather than checked in, because the interesting
// depths are exactly the ones an ordinary encode never produces. `flac` accepts
// them through a WAVE_FORMAT_EXTENSIBLE header naming wValidBitsPerSample, and
// insists the unused low bits are zero -- which is the same left-aligned
// convention this decoder writes on the way back out, arrived at independently.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate = 44100.0;

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-flac-depth-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

/// Everything, not just the FLAC decoder: opening by URL needs the file source
/// as much as it needs something that understands the container.
PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

struct Fixture {
    std::filesystem::path path;
    std::int32_t          peakSample = 0;  ///< largest magnitude actually written
};

/// A mono tone at `validBits`, in the next container up, left-aligned.
///
/// containerBytes is what the WAV stores per sample; validBits is what the
/// samples actually occupy. flac reads the pair and encodes at validBits.
[[nodiscard]] std::optional<Fixture> makeFlac(const std::string& name,
                                              std::uint16_t      validBits,
                                              std::uint16_t      containerBytes) {
    const auto wav  = fixtureDir() / (name + ".wav");
    const auto flac = fixtureDir() / (name + ".flac");

    const std::uint16_t containerBits = containerBytes * 8;
    const std::uint32_t shift         = containerBits - validBits;
    const auto          fullScale     = static_cast<double>(1 << (validBits - 1));

    // Just inside full scale, so clipping cannot be what the peak measures.
    const double  amplitude = fullScale * 0.98;
    constexpr int kFrames   = 4410;

    std::vector<std::byte> samples;
    std::int32_t           peak = 0;
    for (int i = 0; i < kFrames; ++i) {
        const auto value = static_cast<std::int32_t>(
            amplitude * std::sin(xpcog::test::kTwoPi * 440.0 * (i / kRate)));
        peak = std::max(peak, std::abs(value));

        const auto stored = static_cast<std::uint32_t>(value) << shift;
        for (std::uint16_t b = 0; b < containerBytes; ++b) {
            samples.push_back(static_cast<std::byte>((stored >> (8 * b)) & 0xFF));
        }
    }

    const auto u32 = [](std::uint32_t v, std::FILE* f) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [](std::uint16_t v, std::FILE* f) { std::fwrite(&v, 2, 1, f); };

    std::FILE* file = std::fopen(wav.string().c_str(), "wb");
    if (file == nullptr) {
        return std::nullopt;
    }

    constexpr std::uint32_t kFmtBytes = 40;  // 16 + cbSize(2) + 22 of extension
    const auto              dataBytes = static_cast<std::uint32_t>(samples.size());

    std::fwrite("RIFF", 1, 4, file);
    u32(4 + (8 + kFmtBytes) + (8 + dataBytes), file);
    std::fwrite("WAVEfmt ", 1, 8, file);
    u32(kFmtBytes, file);
    u16(0xFFFE, file);  // WAVE_FORMAT_EXTENSIBLE
    u16(1, file);       // mono
    u32(static_cast<std::uint32_t>(kRate), file);
    u32(static_cast<std::uint32_t>(kRate) * containerBytes, file);
    u16(containerBytes, file);  // block align
    u16(containerBits, file);
    u16(22, file);        // cbSize
    u16(validBits, file); // wValidBitsPerSample -- the whole point
    u32(0x4, file);       // SPEAKER_FRONT_CENTER
    // KSDATAFORMAT_SUBTYPE_PCM
    static constexpr unsigned char kPcmGuid[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    std::fwrite(kPcmGuid, 1, sizeof(kPcmGuid), file);
    std::fwrite("data", 1, 4, file);
    u32(dataBytes, file);
    std::fwrite(samples.data(), 1, samples.size(), file);
    std::fclose(file);

    const std::string command = "flac -s -f --totally-silent -o \"" + flac.string() +
                                "\" \"" + wav.string() + "\"" +
                                xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(flac)) {
        return std::nullopt;
    }
    return Fixture{flac, peak};
}

/// Everything the decoder produces, as float.
[[nodiscard]] std::vector<float> decode(const std::filesystem::path& path,
                                        AudioFormat&                 format) {
    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);
    format = opened.decoder->properties().format;

    std::vector<float> pcm;
    AudioChunk         chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t count  = float32SampleCount(chunk);
        const std::size_t offset = pcm.size();
        pcm.resize(offset + count);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, count));
    }
    return pcm;
}

[[nodiscard]] double peakOf(const std::vector<float>& pcm) {
    double peak = 0.0;
    for (const float value : pcm) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return peak;
}

/// A six-second tone encoded as an ordinary 16-bit FLAC, with `cue` attached as
/// its CUESHEET tag. Returns nullopt when `flac` or `metaflac` is missing.
[[nodiscard]] std::optional<std::filesystem::path> makeAlbumFlac(
    const std::string& name, const std::string& cue, int frames) {
    const auto wav   = fixtureDir() / (name + ".wav");
    const auto flac  = fixtureDir() / (name + ".flac");
    const auto sheet = fixtureDir() / (name + ".cue");

    std::vector<std::byte> samples;
    for (int i = 0; i < frames; ++i) {
        const auto value = static_cast<std::int16_t>(
            20000.0 * std::sin(xpcog::test::kTwoPi * 440.0 * (i / kRate)));
        const auto bits = static_cast<std::uint16_t>(value);
        samples.push_back(static_cast<std::byte>(bits & 0xFF));
        samples.push_back(static_cast<std::byte>((bits >> 8) & 0xFF));
    }

    const auto u32 = [](std::uint32_t v, std::FILE* f) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [](std::uint16_t v, std::FILE* f) { std::fwrite(&v, 2, 1, f); };

    std::FILE* file = std::fopen(wav.string().c_str(), "wb");
    if (file == nullptr) {
        return std::nullopt;
    }
    const auto dataBytes = static_cast<std::uint32_t>(samples.size());
    std::fwrite("RIFF", 1, 4, file);
    u32(36 + dataBytes, file);
    std::fwrite("WAVEfmt ", 1, 8, file);
    u32(16, file);
    u16(1, file);
    u16(1, file);
    u32(static_cast<std::uint32_t>(kRate), file);
    u32(static_cast<std::uint32_t>(kRate) * 2, file);
    u16(2, file);
    u16(16, file);
    std::fwrite("data", 1, 4, file);
    u32(dataBytes, file);
    std::fwrite(samples.data(), 1, samples.size(), file);
    std::fclose(file);

    std::FILE* cueFile = std::fopen(sheet.string().c_str(), "wb");
    if (cueFile == nullptr) {
        return std::nullopt;
    }
    std::fwrite(cue.data(), 1, cue.size(), cueFile);
    std::fclose(cueFile);

    const std::string encode = "flac -s -f --totally-silent -o \"" + flac.string() +
                               "\" \"" + wav.string() + "\"" +
                               xpcog::test::kSilenceStderr;
    if (std::system(encode.c_str()) != 0 || !std::filesystem::exists(flac)) {
        return std::nullopt;
    }

    const std::string tag = "metaflac --set-tag-from-file=CUESHEET=\"" +
                            sheet.string() + "\" \"" + flac.string() + "\"" +
                            xpcog::test::kSilenceStderr;
    if (std::system(tag.c_str()) != 0) {
        return std::nullopt;
    }
    return flac;
}

/// Two tracks, the second starting two seconds in.
[[nodiscard]] std::string twoTrackCue() {
    return "FILE \"album.flac\" WAVE\n"
           "  TRACK 01 AUDIO\n"
           "    TITLE \"First Song\"\n"
           "    PERFORMER \"Someone\"\n"
           "    INDEX 01 00:00:00\n"
           "  TRACK 02 AUDIO\n"
           "    TITLE \"Second Song\"\n"
           "    INDEX 01 00:02:00\n";
}

}  // namespace

TEST_CASE("a FLAC with a cue sheet expands into its tracks", "[flac]") {
    const auto album = makeAlbumFlac("album", twoTrackCue(), 44100 * 6);
    if (!album) {
        SKIP("the `flac` and `metaflac` tools are not available");
    }

    const Url              url     = Url::fromLocalPath(*album);
    const std::vector<Url> entries = registry().expandContainer(url);

    // One entry per track, not one for the album. Without this a six-second file
    // holding two songs is added as a single six-second entry.
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].fragment() == "01");
    CHECK(entries[1].fragment() == "02");
}

TEST_CASE("a cue sheet track is its own length, tags and audio", "[flac]") {
    const auto album = makeAlbumFlac("album2", twoTrackCue(), 44100 * 6);
    if (!album) {
        SKIP("the `flac` and `metaflac` tools are not available");
    }

    const Url url = Url::fromLocalPath(*album);

    AudioFormat      wholeFormat{};
    const auto       whole = decode(*album, wholeFormat);
    REQUIRE(whole.size() == 44100 * 6);

    // --- track one: the first two seconds ---
    {
        auto opened = registry().open(url.withFragment("01"));
        REQUIRE(opened);
        const TrackProperties props = opened.decoder->properties();
        CHECK(props.totalFrames == 44100 * 2);

        const MetadataMap tags = opened.decoder->metadata();
        CHECK(tags.first("title") == "First Song");
        CHECK(tags.first("artist") == "Someone");
    }

    // --- track two: from two seconds to the end, and the audio to prove it ---
    {
        auto opened = registry().open(url.withFragment("02"));
        REQUIRE(opened);
        const TrackProperties props = opened.decoder->properties();
        CHECK(props.totalFrames == 44100 * 4);
        CHECK(opened.decoder->metadata().first("title") == "Second Song");

        std::vector<float> pcm;
        AudioChunk         chunk;
        while (opened.decoder->readAudio(chunk)) {
            const std::size_t count  = float32SampleCount(chunk);
            const std::size_t offset = pcm.size();
            pcm.resize(offset + count);
            convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, count));
        }

        // Exactly the track, and exactly the part of the file it names -- the
        // bug was that it began at the album's first sample and ran to its last.
        REQUIRE(pcm.size() == 44100 * 4);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            if (pcm[i] != whole[(44100 * 2) + i]) {
                FAIL("sample " << i << " is not where the sheet says it is");
            }
        }
    }
}

TEST_CASE("a FLAC without a cue sheet is still one track", "[flac]") {
    const auto plain = makeFlac("plain", 16, 2);
    if (!plain) {
        SKIP("the `flac` command-line tool is not available");
    }

    const Url url = Url::fromLocalPath(plain->path);
    REQUIRE(registry().expandContainer(url).size() == 1);
    CHECK(registry().expandContainer(url).front().fragment().empty());
}

TEST_CASE("a 12-bit FLAC plays at the level it was recorded", "[flac]") {
    const auto fixture = makeFlac("depth12", 12, 2);
    if (!fixture) {
        SKIP("the `flac` command-line tool is not available");
    }

    AudioFormat        format{};
    const auto         pcm  = decode(fixture->path, format);
    const double       peak = peakOf(pcm);
    const double expected = static_cast<double>(fixture->peakSample) / 2048.0;  // 2^11

    CHECK(format.bitsPerSample == 12);
    REQUIRE_FALSE(pcm.empty());

    // Sixteen times quieter is what leaving the sample where libFLAC put it
    // sounds like, and it is a plausible-looking waveform rather than silence --
    // which is why this went unnoticed.
    CHECK(peak == Catch::Approx(expected).margin(0.002));
    CHECK(peak > 0.5);
}

TEST_CASE("a 20-bit FLAC plays at the level it was recorded", "[flac]") {
    // The same shortfall, one container up: 20 valid bits inside 24.
    const auto fixture = makeFlac("depth20", 20, 3);
    if (!fixture) {
        SKIP("the `flac` command-line tool is not available");
    }

    AudioFormat  format{};
    const auto   pcm      = decode(fixture->path, format);
    const double peak     = peakOf(pcm);
    const double expected = static_cast<double>(fixture->peakSample) / 524288.0;  // 2^19

    CHECK(format.bitsPerSample == 20);
    REQUIRE_FALSE(pcm.empty());
    CHECK(peak == Catch::Approx(expected).margin(0.002));
    CHECK(peak > 0.5);
}

TEST_CASE("the byte-aligned depths are left alone", "[flac]") {
    // The shift is zero for these, and has to stay zero: a regression here would
    // be every ordinary FLAC playing at the wrong level.
    for (const auto& [bits, bytes] : {std::pair{std::uint16_t{16}, std::uint16_t{2}},
                                      std::pair{std::uint16_t{24}, std::uint16_t{3}}}) {
        const auto fixture =
            makeFlac("depth" + std::to_string(bits), bits, bytes);
        if (!fixture) {
            SKIP("the `flac` command-line tool is not available");
        }

        AudioFormat  format{};
        const auto   pcm = decode(fixture->path, format);
        const double expected =
            static_cast<double>(fixture->peakSample) /
            std::pow(2.0, static_cast<double>(bits - 1));

        INFO("bits = " << bits);
        CHECK(format.bitsPerSample == bits);
        CHECK(peakOf(pcm) == Catch::Approx(expected).margin(0.002));
    }
}
