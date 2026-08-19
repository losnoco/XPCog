// Chained Ogg Vorbis and Opus: the same shape as chained FLAC, and the same two
// answers depending on where the bytes come from.
//
// A file is a container -- every link is a whole track with its own tags and
// length, so it expands to one entry per link. A stream is not: its links arrive
// one at a time, are not known in advance, and must be played straight through
// with the new link's tags announced when it starts. Both libraries chain
// natively, so unlike FLAC the question was never whether these play through; it
// was that a file of them presented as one nameless run of everything, and that
// Opus never said a word when the track changed.

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

constexpr double kRate = 44100.0;

/// The two codecs, and what each needs to be built and recognised.
struct Chained {
    const char* name;
    const char* encoder;    ///< ffmpeg encoder
    const char* extension;  ///< and therefore what the registry matches on
    const char* codecName;  ///< what the decoder reports
};

constexpr Chained kCodecs[] = {
    {"Vorbis", "libvorbis", "ogg", "Ogg Vorbis"},
    {"Opus", "libopus", "opus", "Opus"},
};

/// Three links: two of a second, one of half, at three pitches. The differing
/// length is what makes a per-link duration impossible to confuse with the
/// chain's, and the pitches say which link's audio actually came out.
struct Link {
    const char* title;
    double      frequency;
    double      seconds;
};

constexpr Link kLinks[] = {
    {"Link One", 440.0, 1.0},
    {"Link Two", 880.0, 1.0},
    {"Link Three", 330.0, 0.5},
};

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-ogg-chain-tests";
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

std::filesystem::path writeTone(const std::string& name, double frequency,
                                double seconds) {
    const auto path = fixtureDir() / name;

    const auto                frames = static_cast<int>(kRate * seconds);
    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        samples.push_back(static_cast<std::int16_t>(
            0.6 * 32767.0 * std::sin(xpcog::test::kTwoPi * frequency * t)));
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::FILE* f         = std::fopen(path.string().c_str(), "wb");
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
    return path;
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

/// Three encodes concatenated, which is exactly what a chained file is. Empty
/// when ffmpeg cannot produce this codec.
std::filesystem::path buildChain(const Chained& codec) {
    const auto chained =
        fixtureDir() / (std::string{"chain-"} + codec.encoder + "." + codec.extension);
    if (std::filesystem::exists(chained)) {
        return chained;
    }
    if (!xpcog::test::haveTool("ffmpeg")) {
        return {};
    }

    std::vector<std::filesystem::path> parts;
    for (const Link& link : kLinks) {
        const auto wav = writeTone(std::string{link.title} + ".wav", link.frequency,
                                   link.seconds);
        const auto out = fixtureDir() / (std::string{link.title} + "-" +
                                         codec.encoder + "." + codec.extension);
        const std::string command = std::string{"ffmpeg -y -loglevel error -i \""} +
                                    wav.string() + "\" -c:a " + codec.encoder +
                                    " -metadata title=\"" + link.title + "\"" +
                                    " -metadata artist=\"" + link.title + " Artist\"" +
                                    " \"" + out.string() + "\"" +
                                    xpcog::test::kSilenceStderr;
        if (std::system(command.c_str()) != 0 || !std::filesystem::exists(out)) {
            return {};
        }
        parts.push_back(out);
    }

    std::FILE* out = std::fopen(chained.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    for (const auto& part : parts) {
        const auto bytes = readBytes(part);
        std::fwrite(bytes.data(), 1, bytes.size(), out);
    }
    std::fclose(out);
    return chained;
}

std::vector<float> drain(IDecoder& decoder, double& sampleRate) {
    std::vector<float> pcm;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        sampleRate                = chunk.format().sampleRate;
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = pcm.size();
        pcm.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, samples));
    }
    return pcm;
}

/// A registry whose only source is the unseekable one, so a fixture on disk is
/// reached exactly as a live stream would be -- through the real decoder
/// selection, MultiDecoder included.
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

/// The fixture's path under the unseekable scheme.
Url streamUrl(const std::filesystem::path& path) {
    std::string text = Url::fromLocalPath(path).toString();
    text.replace(0, 4, "teststream");
    return *Url::parse(text);
}

}  // namespace

TEST_CASE("a chained Ogg file expands to one track per link", "[ogg][chained]") {
    for (const Chained& codec : kCodecs) {
        const auto path = buildChain(codec);
        if (path.empty()) {
            WARN("skipping " << codec.name << ": ffmpeg cannot build the fixture");
            continue;
        }
        INFO("codec: " << codec.name);

        const Url              url    = Url::fromLocalPath(path);
        const std::vector<Url> tracks = registry().expandContainer(url);

        REQUIRE(tracks.size() == 3);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            CHECK(tracks[i] == url.withFragment(std::to_string(i)));
        }
    }
}

TEST_CASE("each Ogg chain link decodes as its own track", "[ogg][chained]") {
    for (const Chained& codec : kCodecs) {
        const auto path = buildChain(codec);
        if (path.empty()) {
            continue;
        }
        const Url url = Url::fromLocalPath(path);

        for (std::size_t i = 0; i < std::size(kLinks); ++i) {
            const Link& link = kLinks[i];
            INFO("codec: " << codec.name << " link " << i);

            auto opened = registry().open(url.withFragment(std::to_string(i)));
            REQUIRE(opened);

            const TrackProperties props = opened.decoder->properties();
            CHECK(props.codec == codec.codecName);
            CHECK(props.seekable);

            // Its own tags, from its own comment header.
            CHECK(opened.decoder->metadata().first("title") == link.title);

            // Its own length. Opus always decodes at 48 kHz, so this is checked
            // as a duration rather than a frame count.
            REQUIRE(props.format.sampleRate > 0.0);
            CHECK(static_cast<double>(props.totalFrames) / props.format.sampleRate ==
                  Catch::Approx(link.seconds).margin(0.05));

            // Its own audio, and only its own: reading past the boundary would
            // append the next link's tone to this one.
            double                   rate = props.format.sampleRate;
            const std::vector<float> pcm  = drain(*opened.decoder, rate);
            CHECK(static_cast<double>(pcm.size()) / rate ==
                  Catch::Approx(link.seconds).margin(0.05));
            CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, rate) ==
                  Catch::Approx(link.frequency).margin(15.0));
        }
    }
}

TEST_CASE("seeking within an Ogg chain link stays inside it", "[ogg][chained]") {
    for (const Chained& codec : kCodecs) {
        const auto path = buildChain(codec);
        if (path.empty()) {
            continue;
        }
        INFO("codec: " << codec.name);

        // Seek offsets are the link's own; the file's sample space starts
        // earlier. Getting that wrong lands in a different track entirely, and
        // silently, since every link decodes cleanly wherever you land.
        auto opened = registry().open(Url::fromLocalPath(path).withFragment("1"));
        REQUIRE(opened);

        const double rate = opened.decoder->properties().format.sampleRate;
        REQUIRE(rate > 0.0);
        const auto target = static_cast<std::int64_t>(rate / 2.0);
        REQUIRE(opened.decoder->seek(target) == target);

        double                   observed = rate;
        const std::vector<float> pcm      = drain(*opened.decoder, observed);

        // Half of the second link, at the second link's pitch, and nothing after.
        CHECK(static_cast<double>(pcm.size()) / observed ==
              Catch::Approx(0.5).margin(0.05));
        CHECK(xpcog::test::dominantFrequency(std::span<const float>{pcm}, observed) ==
              Catch::Approx(880.0).margin(15.0));
    }
}

TEST_CASE("a chained Ogg stream plays through and renames as it goes",
          "[ogg][chained]") {
    for (const Chained& codec : kCodecs) {
        const auto path = buildChain(codec);
        if (path.empty()) {
            continue;
        }
        INFO("codec: " << codec.name);

        // The other answer. A stream's links are not known in advance, so there
        // is nothing to expand -- both libraries decode the chain continuously,
        // and the decoder's job is to notice each boundary and say what started.
        auto opened = streamingRegistry().open(streamUrl(path));
        REQUIRE(opened);
        CHECK(opened.decoder->metadata().first("title") == kLinks[0].title);

        std::vector<std::string> announced;
        opened.decoder->setChangeCallback([&](bool, bool metadataChanged) {
            if (metadataChanged) {
                announced.emplace_back(opened.decoder->metadata().first("title"));
            }
        });

        double                   rate = kRate;
        const std::vector<float> pcm  = drain(*opened.decoder, rate);

        // Everything, not one link: a stream is one continuous track.
        CHECK(static_cast<double>(pcm.size()) / rate == Catch::Approx(2.5).margin(0.1));

        // One announcement per boundary, naming the link that started. Without
        // it the tags change inside the decoder and nothing above ever asks
        // again, so a station's new title never reaches the screen.
        CHECK(announced ==
              std::vector<std::string>{kLinks[1].title, kLinks[2].title});
    }
}
