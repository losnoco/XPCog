// Musepack: the parts the conformance table does not reach.
//
// That table already checks the thing most likely to be wrong in a new decoder
// -- per-channel frequency and amplitude against the 440/660 reference -- so
// this covers what is particular to this one.
//
// Seeking is the substantial half. libmpcdec's demuxer answers
// mpc_demux_seek_sample() for both stream versions, but only SV8 carries a seek
// table; SV7 is scanned. mpcenc writes SV8, so what runs here is the indexed
// path, and the check is that the decoder's own idea of position agrees with
// what comes out of the speaker afterwards -- a seek that lands elsewhere and
// then reports the frame it was asked for is the failure worth catching.
//
// The fixture is built by the encoder from ports/libmpcdec rather than by one
// on PATH; see tests/CMakeLists.txt for why there is no such thing to look up.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace xpcog;
namespace fs = std::filesystem;

namespace {

constexpr double kRate     = 44100.0;
constexpr double kToneHz   = 440.0;
constexpr int    kSeconds  = 3;

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

fs::path fixtureDir() {
    static const fs::path dir = [] {
        auto path = fs::temp_directory_path() / "xpcog-musepack-tests";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

/// Three seconds of a steady tone, stereo, at the one rate Musepack's own
/// tables are built for. Long enough that a seek to the middle is unambiguous.
fs::path writeToneWav() {
    const auto out = fixtureDir() / "tone.wav";

    std::vector<std::int16_t> samples;
    const int frames = static_cast<int>(kRate) * kSeconds;
    samples.reserve(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const auto   v = static_cast<std::int16_t>(
            0.6 * 32767.0 * std::sin(xpcog::test::kTwoPi * kToneHz * t));
        samples.push_back(v);
        samples.push_back(v);
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::FILE* f         = std::fopen(out.string().c_str(), "wb");
    REQUIRE(f != nullptr);
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
    return out;
}

/// Empty when this build has no encoder to make one with.
fs::path buildFixture() {
#ifndef XPCOG_MPCENC
    return {};
#else
    const auto out = fixtureDir() / "tone.mpc";
    if (fs::exists(out)) {
        return out;
    }
    const auto        wav = writeToneWav();
    const std::string command = xpcog::test::shellCommand(
        std::string{"\""} + XPCOG_MPCENC + "\" --silent --overwrite --standard \"" +
        wav.string() + "\" \"" + out.string() + "\"" + xpcog::test::kSilenceStderr);
    if (std::system(command.c_str()) != 0 || !fs::exists(out)) {
        return {};
    }
    return out;
#endif
}

std::vector<float> drain(IDecoder& decoder, std::size_t limit) {
    std::vector<float> pcm;
    AudioChunk         chunk;
    while (pcm.size() < limit && decoder.readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t at      = pcm.size();
        pcm.resize(at + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(at, samples));
    }
    return pcm;
}

}  // namespace

TEST_CASE("a Musepack file reports its length and rate", "[musepack]") {
    const auto path = buildFixture();
    if (path.empty()) SKIP("mpcenc did not produce a fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.codec == "Musepack");
    CHECK(props.encoding == "lossy");
    CHECK_FALSE(props.lossless);
    CHECK(props.format.sampleRate == Catch::Approx(kRate));
    CHECK(props.format.channels == 2);
    CHECK(props.seekable);

    // Musepack codes in blocks of 1152 samples and the last one is padded, so
    // the declared length is the encoder's own count rather than the file's
    // rounded-up frame total. A tolerance of one block is what separates
    // "reports the real length" from "reports the padding".
    CHECK(static_cast<double>(props.totalFrames) ==
          Catch::Approx(kRate * kSeconds).margin(1152.0));
}

TEST_CASE("a Musepack file decodes to the tone it was made from", "[musepack]") {
    const auto path = buildFixture();
    if (path.empty()) SKIP("mpcenc did not produce a fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    const std::vector<float> pcm = drain(*opened.decoder, 1'000'000);
    REQUIRE(pcm.size() > static_cast<std::size_t>(kRate));

    // Everything the encoder was given, within a block.
    CHECK(static_cast<double>(pcm.size() / 2) ==
          Catch::Approx(kRate * kSeconds).margin(1152.0));

    // The left channel alone. dominantFrequency counts zero crossings and wants
    // one channel: handing it the interleaved buffer would happen to work here,
    // because both channels carry the same tone and the result reads as one
    // signal at twice the rate, and would stop working the moment the fixture
    // was not symmetric.
    std::vector<float> left;
    left.reserve(pcm.size() / 2);
    for (std::size_t i = 0; i < pcm.size(); i += 2) {
        left.push_back(pcm[i]);
    }
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{left}, kRate) ==
          Catch::Approx(kToneHz).margin(15.0));
}

TEST_CASE("seeking a Musepack file lands where it says it did", "[musepack]") {
    const auto path = buildFixture();
    if (path.empty()) SKIP("mpcenc did not produce a fixture");

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    const auto target = static_cast<std::int64_t>(kRate * 2.0);
    REQUIRE(opened.decoder->seek(target) == target);

    // The tone is steady, so what proves the seek is the *amount* left rather
    // than its pitch: one second of a three-second file. A seek that silently
    // did nothing would hand back three.
    const std::vector<float> pcm = drain(*opened.decoder, 1'000'000);
    CHECK(static_cast<double>(pcm.size() / 2) / kRate ==
          Catch::Approx(1.0).margin(0.1));

    // And seeking back to the start gives the whole file again, which is the
    // half that a decoder holding a stale position passes without.
    REQUIRE(opened.decoder->seek(0) == 0);
    const std::vector<float> whole = drain(*opened.decoder, 1'000'000);
    CHECK(static_cast<double>(whole.size() / 2) / kRate ==
          Catch::Approx(static_cast<double>(kSeconds)).margin(0.1));
}
