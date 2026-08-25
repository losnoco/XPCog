// A stream whose sample rate changes part-way through.
//
// ADTS AAC carries the sampling frequency in every frame header rather than once
// at the top of the file, so a stream is free to change it between frames and
// some do: a station switching bitrate profile, or two encodes concatenated the
// way ADTS invites -- which is a legal file, not a corrupt one. MP4 cannot do
// this and neither can most containers, which is why it stays unnoticed.
//
// FFmpeg reports the new rate on the frame rather than announcing it, so a
// decoder that reads its format once at open goes on labelling the audio with
// the rate the file started at. Nothing sounds broken while decoding -- the
// samples themselves are correct -- but everything downstream resamples from a
// rate that is not the one the samples are at, so the second half plays at the
// wrong speed. Here that is 44100 audio called 24000: it runs at 54% and drops
// most of an octave.
//
// The fixture is two encodes concatenated, because that is byte-for-byte the
// shape the pathological files take. Each half is a tone of its own, so the two
// are told apart by pitch rather than by anything the container says -- and a
// pitch measured against the rate the *chunk* declares is exactly the assertion
// that fails when the label is wrong.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kLowRate   = 24000.0;
constexpr double kHighRate  = 44100.0;
constexpr double kLowTone   = 440.0;
constexpr double kHighTone  = 880.0;
constexpr double kSeconds   = 2.0;

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// See test_ffmpeg_stream_tags.cpp: a function rather than a namespace-scope
/// const bool, so registerAllCodecs() does not run before main.
[[nodiscard]] bool haveFFmpeg() {
    static const bool answer = [] {
        const auto extensions = registry().allExtensions();
        return std::find(extensions.begin(), extensions.end(), "aac") !=
               extensions.end();
    }();
    return answer;
}

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-ffmpeg-ratechange";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::uint8_t> data;
    std::uint8_t              buffer[4096];
    std::size_t               got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        data.insert(data.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return data;
}

/// A stereo tone as a 16-bit WAV, for the encoder to read.
std::filesystem::path writeTone(const std::string& name, double rate, double tone) {
    const auto path   = fixtureDir() / name;
    const auto frames = static_cast<int>(rate * kSeconds);

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const auto   v = static_cast<std::int16_t>(
            0.6 * 32767.0 * std::sin(xpcog::test::kTwoPi * tone * t));
        samples.push_back(v);
        samples.push_back(v);
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::FILE* f         = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 4);
    u16(4); u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);
    return path;
}

/// The rates the ADTS headers themselves declare, in order, one entry per run.
///
/// The test's own premise, checked rather than assumed: an encoder that resampled
/// both halves to one rate would produce a file that passes everything below
/// while testing nothing. Walks the frame-length field rather than searching for
/// sync words, because 0xFFF occurs inside compressed audio too.
std::vector<double> declaredRateRuns(const std::vector<std::uint8_t>& data) {
    static constexpr double kAdtsRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                            24000, 22050, 16000, 12000, 11025, 8000,
                                            7350,  0,     0,     0};
    std::vector<double>     runs;
    std::size_t             position = 0;

    while (position + 7 <= data.size()) {
        if (data[position] != 0xFF || (data[position + 1] & 0xF0) != 0xF0) {
            break;
        }
        const double rate = kAdtsRates[(data[position + 2] >> 2) & 0x0F];
        if (runs.empty() || runs.back() != rate) {
            runs.push_back(rate);
        }
        const std::size_t length =
            (static_cast<std::size_t>(data[position + 3] & 0x03) << 11) |
            (static_cast<std::size_t>(data[position + 4]) << 3) |
            (static_cast<std::size_t>(data[position + 5]) >> 5);
        if (length < 7) {
            break;
        }
        position += length;
    }
    return runs;
}

/// Two ADTS encodes at different rates, concatenated. Empty when ffmpeg is not
/// installed to produce the audio.
std::filesystem::path buildSwitchingStream() {
    const auto joined = fixtureDir() / "switching.aac";
    if (std::filesystem::exists(joined)) {
        return joined;
    }
    if (!xpcog::test::haveTool("ffmpeg")) {
        return {};
    }

    const auto encode = [](const std::filesystem::path& wav,
                           const std::filesystem::path& aac) {
        const std::string command = "ffmpeg -y -loglevel error -i \"" + wav.string() +
                                    "\" -c:a aac -b:a 128k -f adts \"" + aac.string() +
                                    "\"" + xpcog::test::kSilenceStderr;
        return std::system(command.c_str()) == 0 && std::filesystem::exists(aac);
    };

    const auto lowAac  = fixtureDir() / "low.aac";
    const auto highAac = fixtureDir() / "high.aac";
    if (!encode(writeTone("low.wav", kLowRate, kLowTone), lowAac) ||
        !encode(writeTone("high.wav", kHighRate, kHighTone), highAac)) {
        return {};
    }

    std::vector<std::uint8_t> out  = readBytes(lowAac);
    const std::vector<std::uint8_t> high = readBytes(highAac);
    out.insert(out.end(), high.begin(), high.end());

    std::FILE* f = std::fopen(joined.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return joined;
}

/// Everything the decoder produced, split by the rate the chunk carrying it
/// declared, plus the order those rates appeared in.
struct Decoded {
    std::vector<double>             rateRuns;
    std::vector<std::vector<float>> mono;  // one per run
    int                             propertyReports = 0;
    int                             metadataReports = 0;
};

Decoded decodeAll(IDecoder& decoder) {
    Decoded result;
    decoder.setChangeCallback([&](bool properties, bool metadata) {
        result.propertyReports += properties ? 1 : 0;
        result.metadataReports += metadata ? 1 : 0;
    });

    AudioChunk chunk;
    while (decoder.readAudio(chunk)) {
        const double rate     = chunk.format().sampleRate;
        const auto   channels = chunk.format().channels;
        if (result.rateRuns.empty() || result.rateRuns.back() != rate) {
            result.rateRuns.push_back(rate);
            result.mono.emplace_back();
        }

        const auto* samples = reinterpret_cast<const float*>(chunk.bytes().data());
        auto&       into    = result.mono.back();
        for (std::size_t f = 0; f < chunk.frameCount(); ++f) {
            into.push_back(samples[f * channels]);
        }
    }
    return result;
}

}  // namespace

TEST_CASE("a mid-stream sample rate change is followed", "[ffmpeg][ratechange]") {
    if (!haveFFmpeg()) SKIP("the FFmpeg decoder is not built into this configuration");

    const auto path = buildSwitchingStream();
    if (path.empty()) SKIP("ffmpeg not available to build an ADTS fixture");

    // The premise. Two runs in the headers, or the rest of this proves nothing.
    const auto runs = declaredRateRuns(readBytes(path));
    REQUIRE(runs.size() == 2);
    REQUIRE(runs[0] == kLowRate);
    REQUIRE(runs[1] == kHighRate);

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    // What the file opens as, which is the first half only.
    CHECK(opened.decoder->properties().format.sampleRate == kLowRate);

    const Decoded decoded = decodeAll(*opened.decoder);

    // The chunks carry the change; nothing has to be asked.
    REQUIRE(decoded.rateRuns.size() == 2);
    CHECK(decoded.rateRuns[0] == kLowRate);
    CHECK(decoded.rateRuns[1] == kHighRate);

    // And properties() moved with them, once, without claiming a rename.
    CHECK(decoded.propertyReports == 1);
    CHECK(decoded.metadataReports == 0);
    CHECK(opened.decoder->properties().format.sampleRate == kHighRate);

    // The assertion the whole file is for: each half's pitch measured against
    // the rate its own chunks declared. Before the fix the second half was
    // labelled 24000, so this came back at 880 x 24000/44100 -- around 479 Hz.
    REQUIRE(decoded.mono[0].size() > static_cast<std::size_t>(kLowRate));
    REQUIRE(decoded.mono[1].size() > static_cast<std::size_t>(kHighRate));
    CHECK(xpcog::test::dominantFrequency(decoded.mono[0], kLowRate) ==
          Catch::Approx(kLowTone).epsilon(0.05));
    CHECK(xpcog::test::dominantFrequency(decoded.mono[1], kHighRate) ==
          Catch::Approx(kHighTone).epsilon(0.05));

    // Nothing was dropped at the seam beyond the encoder's own priming, which is
    // one AAC frame of 1024 either side.
    CHECK(decoded.mono[0].size() >
          static_cast<std::size_t>(kLowRate * kSeconds) - 4096);
    CHECK(decoded.mono[1].size() >
          static_cast<std::size_t>(kHighRate * kSeconds) - 4096);
}

TEST_CASE("a stream that keeps one rate reports no format change",
          "[ffmpeg][ratechange]") {
    if (!haveFFmpeg()) SKIP("the FFmpeg decoder is not built into this configuration");
    if (buildSwitchingStream().empty()) SKIP("ffmpeg not available to build a fixture");

    // The control. The check above cannot tell "reports a change when there is
    // one" from "reports a change on every frame", and the second would make
    // every ordinary AAC file re-announce itself sixty times a second.
    auto opened = registry().open(Url::fromLocalPath(fixtureDir() / "low.aac"));
    REQUIRE(opened);

    const Decoded decoded = decodeAll(*opened.decoder);
    REQUIRE(decoded.rateRuns.size() == 1);
    CHECK(decoded.rateRuns[0] == kLowRate);
    CHECK(decoded.propertyReports == 0);
}

TEST_CASE("the engine resamples each half of a rate change from its own rate",
          "[ffmpeg][ratechange]") {
    if (!haveFFmpeg()) SKIP("the FFmpeg decoder is not built into this configuration");

    const auto path = buildSwitchingStream();
    if (path.empty()) SKIP("ffmpeg not available to build an ADTS fixture");

    // The decoder emitting the right format is half of it; the other half is the
    // converter following that change without being told. Its resampler is built
    // per chunk from the rate the chunk declares, so this renders the whole file
    // to one device rate and asks whether both halves came out at the pitch they
    // went in at -- which they only can if each was resampled from its own rate.
    RingBuffer  ring(static_cast<std::size_t>(kLowRate * 0.5) * 2);
    auto        output = makeOfflineOutput(ring);
    auto        store  = makeMemorySettingsStore();
    Settings    settings(*store);
    AudioEngine engine(registry(), *output, ring, settings);

    REQUIRE(engine.play(Url::fromLocalPath(path)));
    engine.waitUntilFinished();
    engine.stop();

    // The device runs at the rate the file opened at; the second half is
    // resampled down into it.
    const AudioFormat device = output->negotiatedFormat();
    REQUIRE(device.sampleRate == kLowRate);
    REQUIRE(device.channels >= 1);

    const std::vector<float> captured = capturedAudio(*output);
    const std::size_t        frames   = captured.size() / device.channels;
    REQUIRE(frames > 0);

    // Both halves are present, at the length they would be if each was
    // resampled from its own rate rather than replayed at the other's. Generous:
    // the encoder primes each half and the resampler trims a little at the seam.
    const double seconds = static_cast<double>(frames) / device.sampleRate;
    CHECK(seconds > 2.0 * kSeconds - 0.3);
    CHECK(seconds < 2.0 * kSeconds + 0.3);

    const auto windowOf = [&](double from, double to) {
        std::vector<float> mono;
        const auto begin = static_cast<std::size_t>(from * device.sampleRate);
        const auto end   = std::min(frames, static_cast<std::size_t>(to * device.sampleRate));
        for (std::size_t f = begin; f < end; ++f) {
            mono.push_back(captured[f * device.channels]);
        }
        return mono;
    };

    // Clear of the seam on both sides, which the encoders' priming moves by a
    // few tens of milliseconds either way.
    const std::vector<float> first  = windowOf(0.2, 1.7);
    const std::vector<float> second = windowOf(2.4, 3.6);
    REQUIRE(first.size() > 1000);
    REQUIRE(second.size() > 1000);

    CHECK(xpcog::test::dominantFrequency(first, device.sampleRate) ==
          Catch::Approx(kLowTone).epsilon(0.05));
    // The one that fails without the fix: 44100 audio played out at 24000 comes
    // back at 880 x 24000/44100, about 479 Hz.
    CHECK(xpcog::test::dominantFrequency(second, device.sampleRate) ==
          Catch::Approx(kHighTone).epsilon(0.05));
}
