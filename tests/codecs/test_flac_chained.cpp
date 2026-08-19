// Chained Ogg FLAC: a live stream that starts a new logical bitstream at every
// track change.
//
// This is what an Icecast station serving FLAC does. The encoder ends one Ogg
// stream and begins another, with a fresh serial number, STREAMINFO and Vorbis
// comment -- libFLAC calls those chain links, and by default it stops at the end
// of the first one. On a station that means playback ends when the first song
// does, which is not a decode failure and not a network failure: the decoder
// reports a clean end of stream and the engine believes it.
//
// The fixture is two encodes concatenated, which is byte-for-byte the shape the
// station sends. It is read through a source that reports itself unseekable
// while still allowing the small rewind HttpSource allows, because that is the
// combination the real one presents and it is what decides whether chained
// decoding gets switched on at all.

#include "flac/FlacDecoder.hpp"

#include "xpcog/core/AudioChunk.hpp"
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
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate    = 44100.0;
constexpr int    kSeconds = 1;

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-flac-chain-tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

/// A mono tone, so the two links are told apart by their pitch rather than by
/// anything the container says.
std::filesystem::path writeTone(const std::string& name, double frequency) {
    const auto path = fixtureDir() / name;

    std::vector<std::int16_t> samples;
    const int                 frames = static_cast<int>(kRate) * kSeconds;
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

/// Two Ogg FLAC encodes back to back, which is exactly what a station sends
/// across a track change. Empty when the flac encoder is not installed.
std::filesystem::path buildChainedStream() {
    const auto chained = fixtureDir() / "chained.oga";
    if (std::filesystem::exists(chained)) {
        return chained;
    }
    if (!xpcog::test::haveTool("flac")) {
        return {};
    }

    const auto encode = [](const std::string& name, double frequency,
                           const std::string& title) -> std::filesystem::path {
        const auto wav = writeTone(name + ".wav", frequency);
        const auto out = fixtureDir() / (name + ".oga");
        const std::string command =
            "flac --ogg -s -f --totally-silent -T \"TITLE=" + title +
            "\" -T \"ARTIST=" + title + " Artist\" -o \"" + out.string() + "\" \"" +
            wav.string() + "\"" + xpcog::test::kSilenceStderr;
        if (std::system(command.c_str()) != 0 || !std::filesystem::exists(out)) {
            return {};
        }
        return out;
    };

    const auto first  = encode("first", 440.0, "First Link");
    const auto second = encode("second", 880.0, "Second Link");
    if (first.empty() || second.empty()) {
        return {};
    }

    const auto a = readBytes(first);
    const auto b = readBytes(second);

    std::FILE* out = std::fopen(chained.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    std::fwrite(a.data(), 1, a.size(), out);
    std::fwrite(b.data(), 1, b.size(), out);
    std::fclose(out);
    return chained;
}

/// A file that presents itself the way a live HTTP stream does: seekable() is
/// false, because the response has no length and there is no end to seek to,
/// while a small rewind still works -- HttpSource answers seek() from its ring.
/// That distinction is the whole point: the decoder peeks four bytes for the Ogg
/// magic and rewinds, so a source that refused every seek would fail before
/// reaching the case under test, and one that claimed to be seekable would be a
/// file rather than a stream.
class StreamingFileSource final : public ISource {
public:
    explicit StreamingFileSource(const std::filesystem::path& path)
        : bytes_(readBytes(path)) {}

    bool open(const Url& url) override {
        url_ = url;
        return !bytes_.empty();
    }
    [[nodiscard]] bool seekable() const override { return false; }

    bool seek(std::int64_t offset, int whence) override {
        if (whence == SEEK_END) {
            return false;  // no length to measure against
        }
        const std::int64_t target = (whence == SEEK_CUR) ? position_ + offset : offset;
        if (target < 0 || target > static_cast<std::int64_t>(bytes_.size())) {
            return false;
        }
        position_ = target;
        return true;
    }

    [[nodiscard]] std::int64_t tell() const override { return position_; }

    std::int64_t read(void* out, std::int64_t wanted) override {
        const auto available = static_cast<std::int64_t>(bytes_.size()) - position_;
        const auto take      = std::min(wanted, available);
        if (take <= 0) {
            return 0;
        }
        std::memcpy(out, bytes_.data() + position_, static_cast<std::size_t>(take));
        position_ += take;
        return take;
    }

    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::int64_t              position_ = 0;
    Url                       url_;
};

/// Dominant frequency by zero crossings over the steady middle of a span.
double frequencyOf(std::span<const float> mono) {
    if (mono.size() < 1024) {
        return 0.0;
    }
    const std::size_t begin = mono.size() / 4;
    const std::size_t end   = mono.size() * 3 / 4;

    std::size_t crossings = 0;
    float       previous  = 0.0F;
    for (std::size_t i = begin; i < end; ++i) {
        if (i > begin && ((previous < 0.0F) != (mono[i] < 0.0F))) {
            ++crossings;
        }
        previous = mono[i];
    }
    return static_cast<double>(crossings) * kRate / (2.0 * static_cast<double>(end - begin));
}

}  // namespace

TEST_CASE("a chained Ogg FLAC stream plays past the track change", "[flac][chained]") {
    const auto path = buildChainedStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    StreamingFileSource source{path};
    REQUIRE(source.open(*Url::parse("http://station.example/flac")));

    FlacDecoder decoder;
    REQUIRE(decoder.open(&source));

    std::vector<std::string> announcedTitles;
    decoder.setChangeCallback([&](bool /*properties*/, bool metadataChanged) {
        if (metadataChanged) {
            announcedTitles.emplace_back(decoder.metadata().first("title"));
        }
    });

    CHECK(decoder.properties().format.sampleRate == Catch::Approx(kRate));
    CHECK(decoder.properties().format.channels == 1);
    CHECK(decoder.metadata().first("title") == "First Link");

    std::vector<float> pcm;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = pcm.size();
        pcm.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, samples));
    }

    // Both links, not just the first. Stopping at the boundary yields exactly
    // half of this and looks like a clean end of stream, which is why the bug
    // was invisible until a station outlived one song.
    const auto expected = static_cast<std::size_t>(kRate) * 2;
    CHECK(pcm.size() == Catch::Approx(static_cast<double>(expected)).margin(kRate * 0.05));

    // And it is the *second* link's audio, not more of the first.
    REQUIRE(pcm.size() > static_cast<std::size_t>(kRate));
    const std::span<const float> all{pcm};
    CHECK(frequencyOf(all.subspan(0, static_cast<std::size_t>(kRate))) ==
          Catch::Approx(440.0).margin(15.0));
    CHECK(frequencyOf(all.subspan(static_cast<std::size_t>(kRate))) ==
          Catch::Approx(880.0).margin(15.0));
}

TEST_CASE("a chained link's tags replace the finished track's", "[flac][chained]") {
    const auto path = buildChainedStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    StreamingFileSource source{path};
    REQUIRE(source.open(*Url::parse("http://station.example/flac")));

    FlacDecoder decoder;
    REQUIRE(decoder.open(&source));

    std::vector<std::string> announcedTitles;
    decoder.setChangeCallback([&](bool /*properties*/, bool metadataChanged) {
        if (metadataChanged) {
            announcedTitles.emplace_back(decoder.metadata().first("title"));
        }
    });

    AudioChunk chunk;
    while (decoder.readAudio(chunk)) {
    }

    // Once, when the second link began -- this is what renames the row on a
    // station, and it is announced from readAudio() rather than from the
    // metadata callback because a link's blocks arrive one at a time.
    CHECK(announcedTitles == std::vector<std::string>{"Second Link"});

    const MetadataMap tags = decoder.metadata();
    CHECK(tags.first("title") == "Second Link");

    // Replaced, not appended. Vorbis comments legitimately repeat -- a file may
    // carry several ARTIST lines -- so tags accumulate unless a link boundary
    // clears them, and a station's tag list would grow by a whole track every
    // few minutes with the first song's name still at the front.
    const auto* artists = std::get_if<std::vector<std::string>>(tags.find("artist"));
    REQUIRE(artists != nullptr);
    CHECK(artists->size() == 1);
    CHECK(artists->front() == "Second Link Artist");
}
