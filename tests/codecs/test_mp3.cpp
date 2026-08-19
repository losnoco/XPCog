// The two halves of the MP3 decoder, which are two different decoders.
//
// A file goes through minimp3-ex's stream parser: frames indexed, Xing/LAME
// header read, length and gapless padding known up front. A stream cannot be
// indexed at all and is decoded a frame at a time out of a sliding buffer. The
// conformance harness only ever exercises the first, because it decodes files.
//
// Gapless is the part that fails silently. Padding removed at one end only, or
// not at all, is a click between tracks -- audible on a gapless album, invisible
// on a single track, and never a decode error.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"
#include "../TestSource.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate    = 44100.0;
constexpr double kTone    = 440.0;
constexpr int    kFrames  = 44100;  // one second in

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-mp3-tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
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

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::uint8_t> data;
    std::uint8_t              buffer[8192];
    std::size_t               got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        data.insert(data.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return data;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

/// A mono tone encoded by LAME, which writes the Xing/Info header that carries
/// its own gapless padding.
std::filesystem::path buildMp3() {
    const auto mp3 = fixtureDir() / "tone.mp3";
    if (std::filesystem::exists(mp3)) {
        return mp3;
    }
    if (!xpcog::test::haveTool("lame")) {
        return {};
    }

    const auto wav = fixtureDir() / "tone.wav";
    {
        std::vector<std::int16_t> samples;
        samples.reserve(static_cast<std::size_t>(kFrames));
        for (int i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            samples.push_back(static_cast<std::int16_t>(
                0.6 * 32767.0 * std::sin(xpcog::test::kTwoPi * kTone * t)));
        }

        const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
        std::FILE* f         = std::fopen(wav.string().c_str(), "wb");
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
    }

    const std::string command = "lame --quiet -b 192 \"" + wav.string() + "\" \"" +
                                mp3.string() + "\"" + xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(mp3)) {
        return {};
    }
    return mp3;
}

/// An ID3v2.4 tag holding one COMM frame described as `iTunSMPB`, which is where
/// iTunes puts gapless information and where minimp3 never looks.
std::vector<std::uint8_t> itunesGaplessTag(const std::string& value) {
    std::vector<std::uint8_t> payload{0x03, 'e', 'n', 'g'};  // UTF-8, language
    const std::string         description = "iTunSMPB";
    payload.insert(payload.end(), description.begin(), description.end());
    payload.push_back(0x00);
    payload.insert(payload.end(), value.begin(), value.end());

    const auto syncsafe = [](std::size_t size, std::vector<std::uint8_t>& out) {
        out.push_back(static_cast<std::uint8_t>((size >> 21) & 0x7F));
        out.push_back(static_cast<std::uint8_t>((size >> 14) & 0x7F));
        out.push_back(static_cast<std::uint8_t>((size >> 7) & 0x7F));
        out.push_back(static_cast<std::uint8_t>(size & 0x7F));
    };

    std::vector<std::uint8_t> frame{'C', 'O', 'M', 'M'};
    syncsafe(payload.size(), frame);
    frame.insert(frame.end(), {0x00, 0x00});
    frame.insert(frame.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> tag{'I', 'D', '3', 0x04, 0x00, 0x00};
    syncsafe(frame.size(), tag);
    tag.insert(tag.end(), frame.begin(), frame.end());
    return tag;
}

std::vector<float> drain(IDecoder& decoder) {
    std::vector<float> pcm;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = pcm.size();
        pcm.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, samples));
    }
    return pcm;
}

/// A registry whose only source is the unseekable one, so a file on disk arrives
/// exactly as a live stream would -- through real decoder selection.
constexpr std::string_view kStreamScheme[] = {"teststream"};

PluginRegistry& streamingRegistry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        instance.addSource({
            .name    = "TestStreamSource",
            .schemes = kStreamScheme,
            .create  = []() -> SourcePtr {
                return std::make_unique<xpcog::test::StreamingFileSource>();
            },
        });
        registerAllCodecs(instance);  // freezes
        return true;
    }();
    (void)once;
    return instance;
}

Url streamUrl(const std::filesystem::path& path) {
    std::string text = Url::fromLocalPath(path).toString();
    text.replace(0, 4, "teststream");
    return *Url::parse(text);
}

}  // namespace

TEST_CASE("LAME gapless padding is removed", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    // A second in, a second out. An MP3 encoder pads both ends -- a raw decode
    // of this file yields roughly 1100 samples more than went in, which is the
    // click between two tracks of a gapless album. minimp3-ex reads LAME's
    // Xing/Info header and trims it, and this is the check that it is wired up.
    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.codec == "MP3");
    CHECK(props.format.sampleRate == Catch::Approx(kRate));
    CHECK(props.format.channels == 1);
    CHECK(props.seekable);
    CHECK(props.totalFrames == kFrames);

    const std::vector<float> pcm = drain(*opened.decoder);
    CHECK(pcm.size() == static_cast<std::size_t>(kFrames));
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kTone).margin(15.0));
}

TEST_CASE("iTunes gapless information overrides the encoder's", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    // iTunes writes its own gapless numbers into the ID3v2 tag rather than the
    // Xing header, and minimp3 skips the tag without reading it. So the decoder
    // parses it: the fields are the leading padding, the trailing padding and
    // the true length, in hex.
    //
    // The length here is deliberately not the file's real one. Both sources of
    // gapless information are present and they disagree, so the decoded length
    // says which one was believed -- a test using the same number for both would
    // pass without the tag being read at all.
    constexpr std::int64_t kDeclared = 40000;
    const auto             tagged    = fixtureDir() / "itunes.mp3";
    {
        const auto tag   = itunesGaplessTag(" 00000000 00000210 00000AC0 "
                                            "0000000000009C40 00000000 0000000000000100");
        auto       bytes = tag;
        const auto audio = readBytes(path);
        bytes.insert(bytes.end(), audio.begin(), audio.end());
        writeBytes(tagged, bytes);
    }

    auto opened = registry().open(Url::fromLocalPath(tagged));
    REQUIRE(opened);
    CHECK(opened.decoder->properties().totalFrames == kDeclared);

    const std::vector<float> pcm = drain(*opened.decoder);
    CHECK(pcm.size() == static_cast<std::size_t>(kDeclared));
    // Still the tone, so the padding came off the ends rather than the middle.
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kTone).margin(15.0));
}

TEST_CASE("a nonsensical iTunes gapless tag is ignored", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    // Every field is bounded before use, and each bound gets its own fixture:
    // a tag that breaks two of them at once would pass this test with either
    // check removed. Believing a bad tag truncates the track or opens it with
    // silence, both of which read as a bad rip rather than a bad parse.
    struct Bad {
        const char* what;
        const char* value;
    };
    const Bad cases[] = {
        {"a start padding larger than the format can express",
         " 00000000 0FFFFFFF 00000AC0 0000000000009C40 00000000 0000000000000100"},
        {"a last-frames offset past the end of the file",
         " 00000000 00000210 00000AC0 0000000000009C40 00000000 00000000FFFFFFFF"},
        {"an end padding below the decoder's own delay",
         " 00000000 00000210 00000100 0000000000009C40 00000000 0000000000000100"},
    };

    for (const Bad& bad : cases) {
        INFO(bad.what);

        const auto tagged = fixtureDir() / "nonsense.mp3";
        {
            auto       bytes = itunesGaplessTag(bad.value);
            const auto audio = readBytes(path);
            bytes.insert(bytes.end(), audio.begin(), audio.end());
            writeBytes(tagged, bytes);
        }

        auto opened = registry().open(Url::fromLocalPath(tagged));
        REQUIRE(opened);

        // Checked, and not incidental: an out-of-range padding makes this
        // decoder skip past the end of the file and fail to open, at which
        // point MultiDecoder quietly hands the file to FFmpeg -- which reports
        // the same frame count from the same Xing header. Without naming the
        // codec, the test passes whether the tag was rejected or the decoder
        // fell over.
        CHECK(opened.decoder->properties().codec == "MP3");
        CHECK(opened.decoder->properties().totalFrames == kFrames);
        CHECK(drain(*opened.decoder).size() == static_cast<std::size_t>(kFrames));
    }
}

TEST_CASE("iTunes padding is measured from the decoded signal", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    // The two gapless conventions do not count from the same place. LAME's
    // header gives the encoder delay, measured from the first frame; iTunes
    // gives padding measured from the start of the *decoded* signal, which is
    // 528 samples of decoder delay plus one further along. Use one convention
    // with the other's numbers and the track moves by twelve milliseconds --
    // audible as a click on a gapless album, and invisible to any test that
    // counts frames or measures pitch, since neither changes.
    //
    // So: an iTunSMPB naming LAME's own encoder delay must decode to exactly
    // what LAME's own header produces. 576 is LAME's documented default delay,
    // not a number fitted to this decoder.
    auto reference = registry().open(Url::fromLocalPath(path));
    REQUIRE(reference);
    const std::vector<float> expected = drain(*reference.decoder);
    REQUIRE(expected.size() == static_cast<std::size_t>(kFrames));

    const auto tagged = fixtureDir() / "delay.mp3";
    {
        auto       bytes = itunesGaplessTag(" 00000000 00000240 00000240 "
                                            "000000000000AC44 00000000 0000000000000100");
        const auto audio = readBytes(path);
        bytes.insert(bytes.end(), audio.begin(), audio.end());
        writeBytes(tagged, bytes);
    }

    auto opened = registry().open(Url::fromLocalPath(tagged));
    REQUIRE(opened);
    CHECK(opened.decoder->properties().codec == "MP3");
    REQUIRE(opened.decoder->properties().totalFrames == kFrames);

    const std::vector<float> actual = drain(*opened.decoder);
    REQUIRE(actual.size() == expected.size());

    // Sample for sample. Both decodes came from the same bytes through the same
    // decoder, so the only thing that can differ is where they started.
    double worst = 0.0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(actual[i] - expected[i])));
    }
    CHECK(worst == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("seeking an MP3 lands on the right audio", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    auto whole = registry().open(Url::fromLocalPath(path));
    REQUIRE(whole);
    const std::vector<float> reference = drain(*whole.decoder);
    REQUIRE(reference.size() == static_cast<std::size_t>(kFrames));

    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);
    const std::int64_t target = kFrames / 2;
    REQUIRE(opened.decoder->seek(target) == target);

    const std::vector<float> after = drain(*opened.decoder);
    CHECK(static_cast<double>(after.size()) ==
          Catch::Approx(static_cast<double>(kFrames - target)).margin(kRate * 0.05));

    // Content, not just a frame count: a seek that reports the right position
    // and delivers the wrong samples is what gapless bookkeeping gets wrong, and
    // it decodes perfectly cleanly while doing it.
    const std::size_t compare =
        std::min(after.size(), reference.size() - static_cast<std::size_t>(target));
    REQUIRE(compare > 4096);
    double worst = 0.0;
    for (std::size_t i = 0; i < compare; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(
                                    after[i] - reference[static_cast<std::size_t>(target) + i])));
    }
    // Not exact: MP3 frames overlap, so a decode starting mid-file rebuilds the
    // first block from less history than a decode that reached it in sequence.
    CHECK(worst < 0.1);
}

TEST_CASE("an MP3 stream decodes without an index", "[mp3]") {
    const auto path = buildMp3();
    if (path.empty()) SKIP("lame is not available to build a fixture");

    // The other decoder. No seeking back, no end to scan to, so no frame index
    // and no Xing header applied -- just frames out of a sliding buffer. It is
    // the only path an Icecast MP3 station ever takes, and the conformance
    // harness never reaches it because that decodes files.
    auto opened = streamingRegistry().open(streamUrl(path));
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.codec == "MP3");
    CHECK(props.format.sampleRate == Catch::Approx(kRate));
    CHECK(props.format.channels == 1);
    // Unknowable without an index, and saying otherwise puts a length on the
    // seek bar that cannot be reached.
    CHECK_FALSE(props.seekable);
    CHECK(props.totalFrames == 0);
    CHECK(opened.decoder->seek(1000) == -1);

    const std::vector<float> pcm = drain(*opened.decoder);
    // The whole file, plus the encoder padding nothing trimmed: a stream has no
    // Xing header to read it from.
    CHECK(static_cast<double>(pcm.size()) / kRate == Catch::Approx(1.0).margin(0.1));
    CHECK(pcm.size() >= static_cast<std::size_t>(kFrames));
    CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, kRate) ==
          Catch::Approx(kTone).margin(15.0));
}

#ifdef XPCOG_FATE_SUITE
TEST_CASE("real gapless files decode to their declared lengths", "[mp3][corpus]") {
    // Synthetic fixtures prove the parse; they cannot prove the field layout,
    // because the test wrote it. These are real encodes from FFmpeg's FATE
    // suite, and both expectations come from outside this code:
    //
    //   * the LAME file's length is what ffmpeg itself decodes it to;
    //   * the iTunes file's is the number written in its own iTunSMPB field,
    //     read out of the file by hand.
    //
    // The second is the interesting one. ffmpeg decodes it to 421632 frames --
    // 2682 more -- because it applies the Xing delay and never reads iTunSMPB.
    // That difference is the entire reason this parsing exists, so a test that
    // simply compared against ffmpeg would call the bug correct.
    struct Expected {
        const char*  file;
        std::int64_t frames;
        const char*  source;
    };
    const Expected cases[] = {
        {"gapless/gapless.mp3", 682880, "agrees with ffmpeg"},
        {"gapless/gapless-itunes.mp3", 418950, "the file's own iTunSMPB field"},
    };

    int checked = 0;
    for (const Expected& want : cases) {
        const auto path = std::filesystem::path{XPCOG_FATE_SUITE} / want.file;
        if (!std::filesystem::exists(path)) {
            WARN("missing FATE fixture: " << path.string());
            continue;
        }
        INFO(want.file << " (" << want.source << ")");
        ++checked;

        auto opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);
        CHECK(opened.decoder->properties().codec == "MP3");
        CHECK(opened.decoder->properties().totalFrames == want.frames);

        // Decoded, not just reported: the length is trimmed by three separate
        // fields and only playing it out proves they agree.
        CHECK(drain(*opened.decoder).size() ==
              static_cast<std::size_t>(want.frames) *
                  opened.decoder->properties().format.channels);
    }

    if (checked == 0) {
        WARN("XPCOG_FATE_SUITE is set but held none of the gapless fixtures");
    }
}
#endif
