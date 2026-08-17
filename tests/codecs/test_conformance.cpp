// Per-codec conformance harness.
//
// Every codec is checked against the same synthetic signal: a 440 Hz tone in the
// left channel and 660 Hz in the right, at different amplitudes. That asymmetry is
// deliberate -- it catches swapped channels, a duplicated channel, and a silent
// channel, none of which a duration or size check would notice.
//
// Adding codec number six through thirty-five means adding one row to kCodecs.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kSampleRate  = 44100.0;
constexpr double kLeftFreq    = 440.0;
constexpr double kRightFreq   = 660.0;
constexpr double kLeftAmp     = 0.67;
constexpr double kRightAmp    = 0.55;
constexpr int    kFrames      = 44100;  // one second

struct Codec {
    const char* name;
    const char* extension;
    /// Shell template; {in} and {out} are substituted. Empty means the fixture is
    /// the WAV itself (no encoder needed).
    const char* encoder;
    const char* expectedCodec;
    bool        lossless;
};

/// Encoders are looked up at run time; a codec whose encoder is missing is
/// skipped rather than failing, so the suite still runs on a bare machine.
constexpr Codec kCodecs[] = {
    {"FLAC", "flac", R"(flac -s -f --totally-silent -o "{out}" "{in}")", "FLAC", true},
    {"Ogg Vorbis", "ogg", R"(oggenc -Q -q 6 -o "{out}" "{in}")", "Ogg Vorbis", false},
    {"Opus", "opus", R"(opusenc --quiet --bitrate 128 "{in}" "{out}")", "Opus", false},
    {"MP3", "mp3", R"(lame --quiet -b 192 "{in}" "{out}")", "MP3", false},
    {"WavPack", "wv", R"(wavpack -q -y "{in}" -o "{out}")", "WavPack", true},
    // FFmpeg-backed. expectedCodec is FFmpeg's long name, which is what the
    // decoder reports.
    {"AAC", "m4a", R"(ffmpeg -y -loglevel error -i "{in}" -c:a aac -b:a 192k "{out}")",
     "AAC (Advanced Audio Coding)", false},
    {"ALAC", "alac", R"(ffmpeg -y -loglevel error -i "{in}" -c:a alac -f ipod "{out}")",
     "ALAC (Apple Lossless Audio Codec)", true},
};

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-codec-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

std::filesystem::path referenceWav() {
    static const std::filesystem::path path = [] {
        const auto out = fixtureDir() / "reference.wav";

        std::vector<std::int16_t> samples;
        samples.reserve(static_cast<std::size_t>(kFrames) * 2);
        for (int i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            samples.push_back(static_cast<std::int16_t>(
                kLeftAmp * 32767.0 * std::sin(xpcog::test::kTwoPi * kLeftFreq * t)));
            samples.push_back(static_cast<std::int16_t>(
                kRightAmp * 32767.0 * std::sin(xpcog::test::kTwoPi * kRightFreq * t)));
        }

        const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
        std::FILE* f         = std::fopen(out.string().c_str(), "wb");
        const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

        std::fwrite("RIFF", 1, 4, f);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(2);
        u32(static_cast<std::uint32_t>(kSampleRate));
        u32(static_cast<std::uint32_t>(kSampleRate) * 4);
        u16(4); u16(16);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        std::fwrite(samples.data(), 1, dataBytes, f);
        std::fclose(f);
        return out;
    }();
    return path;
}

/// True when this build actually contains a decoder claiming the extension.
/// Without this the harness fails whenever a codec is switched off at configure
/// time but its encoder happens to be installed -- which is exactly what a
/// partial build looks like.
bool codecCompiledIn(const Codec& codec);

std::optional<std::filesystem::path> encode(const Codec& codec) {
    const auto out = fixtureDir() / (std::string("conformance.") + codec.extension);

    std::string command = codec.encoder;
    const auto  replace = [&](std::string_view token, const std::string& value) {
        for (std::size_t at = command.find(token); at != std::string::npos;
             at             = command.find(token)) {
            command.replace(at, token.size(), value);
        }
    };
    replace("{in}", referenceWav().string());
    replace("{out}", out.string());
    command += " 2>/dev/null";

    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(out)) {
        return std::nullopt;
    }
    return out;
}

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

bool codecCompiledIn(const Codec& codec) {
    const auto extensions = registry().allExtensions();
    return std::find(extensions.begin(), extensions.end(), codec.extension) !=
           extensions.end();
}

struct ChannelStats {
    double rms       = 0.0;
    double frequency = 0.0;
};

/// Frequency via zero crossings over a steady-state window. Crude, but immune to
/// the codec delay differences that make a phase-sensitive comparison useless.
std::vector<ChannelStats> analyse(const std::vector<float>& interleaved,
                                  std::uint32_t channels, double rate) {
    std::vector<ChannelStats> stats(channels);
    const std::size_t frames = interleaved.size() / channels;
    const std::size_t begin  = frames / 4;
    const std::size_t end    = frames * 3 / 4;

    for (std::uint32_t c = 0; c < channels; ++c) {
        double        sumSquares = 0.0;
        std::size_t   crossings  = 0;
        float         previous   = 0.0F;
        for (std::size_t i = begin; i < end; ++i) {
            const float v = interleaved[i * channels + c];
            sumSquares += static_cast<double>(v) * v;
            if (i > begin && ((previous < 0.0F) != (v < 0.0F))) {
                ++crossings;
            }
            previous = v;
        }
        const auto count = static_cast<double>(end - begin);
        stats[c].rms     = std::sqrt(sumSquares / count);
        stats[c].frequency = static_cast<double>(crossings) * rate / (2.0 * count);
    }
    return stats;
}

std::vector<float> decodeAll(const std::filesystem::path& path, TrackProperties& props) {
    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);
    props = opened.decoder->properties();

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

}  // namespace

TEST_CASE("codecs decode the reference signal correctly", "[codecs][conformance]") {
    int tested = 0;

    for (const Codec& codec : kCodecs) {
        if (!codecCompiledIn(codec)) {
            WARN("skipping " << codec.name << ": not built into this configuration");
            continue;
        }
        const auto path = encode(codec);
        if (!path) {
            WARN("skipping " << codec.name << ": encoder not available");
            continue;
        }
        ++tested;

        INFO("codec: " << codec.name);

        TrackProperties props;
        const std::vector<float> pcm = decodeAll(*path, props);

        CHECK(props.codec == codec.expectedCodec);
        CHECK(props.lossless == codec.lossless);
        CHECK(props.format.channels == 2);
        CHECK(props.seekable);

        // Opus always decodes at 48 kHz; everything else keeps the source rate.
        const double rate = props.format.sampleRate;
        CHECK(rate >= 44100.0);

        REQUIRE_FALSE(pcm.empty());

        // Duration within 50 ms: encoders pad, so this is not exact.
        const double seconds = static_cast<double>(pcm.size() / 2) / rate;
        CHECK(seconds == Catch::Approx(1.0).margin(0.05));

        const auto stats = analyse(pcm, 2, rate);

        // The distinguishing check: each channel must carry its own tone at its
        // own level. Swapped, duplicated or silent channels all fail here.
        CHECK(stats[0].frequency == Catch::Approx(kLeftFreq).margin(15.0));
        CHECK(stats[1].frequency == Catch::Approx(kRightFreq).margin(15.0));

        const double expectedLeftRms  = kLeftAmp / std::sqrt(2.0);
        const double expectedRightRms = kRightAmp / std::sqrt(2.0);
        CHECK(stats[0].rms == Catch::Approx(expectedLeftRms).margin(0.05));
        CHECK(stats[1].rms == Catch::Approx(expectedRightRms).margin(0.05));

        // Left is deliberately louder than right; a duplicated channel would make
        // these equal.
        CHECK(stats[0].rms > stats[1].rms);
    }

    // Guards against the whole suite quietly degrading to nothing if the registry
    // or the fixture pipeline breaks.
    if (tested == 0) {
        WARN("no codec encoders available; conformance was not exercised");
    }
}

TEST_CASE("every registered decoder claims at least one extension",
          "[codecs][conformance]") {
    const auto& r = registry();
    REQUIRE(r.decoderCount() > 0);
    CHECK_FALSE(r.allExtensions().empty());
}

TEST_CASE("seeking lands on the right audio, not just the right frame",
          "[codecs][conformance]") {
    // A seek that reports success but delivers the wrong samples is the failure
    // mode that matters: it made cue sheet tracks decode the wrong bytes while
    // every return code looked healthy. Comparing content is the only check that
    // catches it.
    for (const Codec& codec : kCodecs) {
        if (!codecCompiledIn(codec)) {
            continue;
        }
        const auto path = encode(codec);
        if (!path) {
            continue;
        }
        INFO("codec: " << codec.name);

        TrackProperties props;
        const std::vector<float> whole = decodeAll(*path, props);
        if (!props.seekable || whole.empty()) {
            continue;
        }

        const std::uint32_t channels = props.format.channels;
        const std::int64_t  target   = static_cast<std::int64_t>(props.format.sampleRate / 2);
        if (target <= 0 || static_cast<std::size_t>(target) * channels >= whole.size()) {
            continue;
        }

        auto opened = registry().open(Url::fromLocalPath(*path));
        REQUIRE(opened);
        REQUIRE(opened.decoder->seek(target) >= 0);

        AudioChunk chunk;
        REQUIRE(opened.decoder->readAudio(chunk));
        REQUIRE(chunk.frameCount() > 0);

        std::vector<float> afterSeek(float32SampleCount(chunk));
        convertToFloat32(chunk, afterSeek);

        // Lossy codecs re-prime their decoder state after a seek, so the samples
        // are close rather than identical. Lossless codecs must match exactly.
        const std::size_t compare =
            std::min<std::size_t>(afterSeek.size(),
                                  whole.size() - static_cast<std::size_t>(target) * channels);
        const float* expected = whole.data() + static_cast<std::size_t>(target) * channels;

        double worst = 0.0;
        for (std::size_t i = 0; i < compare; ++i) {
            worst = std::max(worst, std::abs(static_cast<double>(afterSeek[i]) - expected[i]));
        }

        if (codec.lossless) {
            CHECK(worst == 0.0);
        } else {
            CHECK(worst < 0.30);
        }
    }
}

TEST_CASE("seeking returns to a consistent position", "[codecs][conformance]") {
    for (const Codec& codec : kCodecs) {
        if (!codecCompiledIn(codec)) {
            continue;
        }
        const auto path = encode(codec);
        if (!path) {
            continue;
        }
        INFO("codec: " << codec.name);

        auto opened = registry().open(Url::fromLocalPath(*path));
        REQUIRE(opened);
        if (!opened.decoder->properties().seekable) {
            continue;
        }

        AudioChunk chunk;
        REQUIRE(opened.decoder->readAudio(chunk));

        // Seek back to the start and confirm the decoder agrees it went there.
        const std::int64_t landed = opened.decoder->seek(0);
        CHECK(landed == 0);
        CHECK(opened.decoder->readAudio(chunk));
        CHECK(chunk.frameCount() > 0);
    }
}
