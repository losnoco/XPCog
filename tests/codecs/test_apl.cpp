// Monkey's Audio Link files: that a range within an image plays as its own
// track, and that the range is the one the link asked for.
//
// The image here is FLAC, not APE, and deliberately. An `.apl` names a file and
// a span of samples in it; the decoder opens that file through the registry and
// has no idea what codec answers. Testing it over FLAC checks exactly the same
// code as testing it over Monkey's Audio would, and does it with an encoder
// every platform already has -- where APE fixtures would need `mac`, which none
// of them do.
//
// The image is two tones back to back, which is what makes a range check mean
// something: a link over the second half that decoded the first would still
// report the right duration and the right sample count.

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

constexpr double kRate      = 44100.0;
constexpr double kFirstHz   = 440.0;
constexpr double kSecondHz  = 880.0;
constexpr int    kHalfFrames = 44100;  // one second each

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
        auto path = fs::temp_directory_path() / "xpcog-apl-tests";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

/// Two seconds: 440 Hz then 880 Hz, mono, one second each.
fs::path writeTwoToneWav() {
    const auto out = fixtureDir() / "image.wav";

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(kHalfFrames) * 2);
    for (int half = 0; half < 2; ++half) {
        const double hz = (half == 0) ? kFirstHz : kSecondHz;
        for (int i = 0; i < kHalfFrames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            samples.push_back(static_cast<std::int16_t>(
                0.6 * 32767.0 * std::sin(xpcog::test::kTwoPi * hz * t)));
        }
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::FILE* f         = std::fopen(out.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1);
    u32(static_cast<std::uint32_t>(kRate));
    u32(static_cast<std::uint32_t>(kRate) * 2);
    u16(2); u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);
    return out;
}

/// The image, encoded. Empty when there is no flac to encode it with.
fs::path buildImage() {
    const auto out = fixtureDir() / "image.flac";
    if (fs::exists(out)) {
        return out;
    }
    if (!xpcog::test::haveTool("flac")) {
        return {};
    }
    const auto        wav = writeTwoToneWav();
    const std::string command = "flac -s -f --totally-silent -o \"" + out.string() +
                                "\" \"" + wav.string() + "\"" +
                                xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !fs::exists(out)) {
        return {};
    }
    return out;
}

/// Writes an `.apl` beside the image. `image` is written into the file as given,
/// so a test can hand it a bare filename to exercise relative resolution.
fs::path writeApl(const std::string& name, const std::string& image,
                  std::int64_t startBlock, std::int64_t finishBlock,
                  bool withTagBanner = true) {
    const auto out = fixtureDir() / name;
    std::FILE* f   = std::fopen(out.string().c_str(), "wb");
    REQUIRE(f != nullptr);

    // CRLF throughout, which is what the ripper writes and what the parser has
    // to tolerate -- a reader that kept the \r would put it in the filename.
    std::string text = "[Monkey's Audio Image Link File]\r\n";
    text += "Image File=" + image + "\r\n";
    text += "Start Block=" + std::to_string(startBlock) + "\r\n";
    text += "Finish Block=" + std::to_string(finishBlock) + "\r\n";
    if (withTagBanner) {
        text += "----- APE TAG (DO NOT TOUCH!!!) -----\r\n";
        text += "Title=Not A Field\r\n";
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return out;
}

std::vector<float> drainAll(IDecoder& decoder) {
    std::vector<float> pcm;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t at      = pcm.size();
        pcm.resize(at + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(at, samples));
    }
    return pcm;
}

}  // namespace

TEST_CASE("an APL plays the range it names, not the whole image", "[apl]") {
    const auto image = buildImage();
    if (image.empty()) SKIP("the flac encoder is not available to build a fixture");

    // The second half, by its own filename: an .apl sits beside its image and
    // names it relatively, which is the only form a ripper writes.
    const auto link = writeApl("second.apl", image.filename().string(), kHalfFrames,
                               kHalfFrames * 2);

    auto opened = registry().open(Url::fromLocalPath(link));
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.totalFrames == kHalfFrames);
    CHECK(props.format.sampleRate == Catch::Approx(kRate));
    CHECK(props.seekable);
    // The codec reported is the image's, since that is what is being decoded.
    CHECK(props.codec == "FLAC");

    const std::vector<float> pcm = drainAll(*opened.decoder);
    CHECK(static_cast<double>(pcm.size()) == Catch::Approx(kHalfFrames).margin(64));

    // The half that matters: 880, not 440. A link that ignored Start Block would
    // pass every check above and fail this one.
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kSecondHz).margin(15.0));
}

TEST_CASE("an APL with no finish block runs to the end of the image", "[apl]") {
    const auto image = buildImage();
    if (image.empty()) SKIP("the flac encoder is not available to build a fixture");

    // Finish Block 0, which Cog reads as "not stated" rather than as an empty
    // range -- its test is `endBlock > startBlock`, and this follows it.
    const auto link = writeApl("open-ended.apl", image.filename().string(),
                               kHalfFrames, 0);

    auto opened = registry().open(Url::fromLocalPath(link));
    REQUIRE(opened);
    CHECK(opened.decoder->properties().totalFrames == kHalfFrames);

    const std::vector<float> pcm = drainAll(*opened.decoder);
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kSecondHz).margin(15.0));
}

TEST_CASE("an APL seeks within its own range", "[apl]") {
    const auto image = buildImage();
    if (image.empty()) SKIP("the flac encoder is not available to build a fixture");

    const auto link = writeApl("seekable.apl", image.filename().string(), kHalfFrames,
                               kHalfFrames * 2);

    auto opened = registry().open(Url::fromLocalPath(link));
    REQUIRE(opened);

    // Frame zero of the link is frame kHalfFrames of the image, so seeking to
    // the link's midpoint must still be the second tone and must leave half a
    // second. A decoder that passed the frame straight through would land in
    // the first tone instead.
    const auto target = static_cast<std::int64_t>(kHalfFrames / 2);
    REQUIRE(opened.decoder->seek(target) == target);

    const std::vector<float> pcm = drainAll(*opened.decoder);
    CHECK(static_cast<double>(pcm.size()) / kRate == Catch::Approx(0.5).margin(0.05));
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kSecondHz).margin(15.0));
}

TEST_CASE("a file that is not an APL is refused", "[apl]") {
    const auto image = buildImage();
    if (image.empty()) SKIP("the flac encoder is not available to build a fixture");

    // The extension alone must not be enough. Without the header check this
    // opens the registry on whatever the first line happens to say.
    const auto stray = fixtureDir() / "stray.apl";
    std::FILE* f     = std::fopen(stray.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const std::string text = "just some text\r\nImage File=" +
                             image.filename().string() + "\r\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);

    CHECK_FALSE(registry().open(Url::fromLocalPath(stray)));
}

TEST_CASE("the APL decoder claims its extension", "[apl]") {
    CHECK(registry().isPlayableExtension("apl"));
}
