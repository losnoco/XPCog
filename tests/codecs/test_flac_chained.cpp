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

#include "xpcog/core/PluginRegistry.hpp"

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
std::filesystem::path writeTone(const std::string& name, double frequency,
                                double frameCount) {
    const auto path = fixtureDir() / name;

    std::vector<std::int16_t> samples;
    const int                 frames = static_cast<int>(frameCount);
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
                           const std::string& title,
                           double frames) -> std::filesystem::path {
        const auto wav = writeTone(name + ".wav", frequency, frames);
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

    // Three, and the last a different length: two boundaries rather than one,
    // and a length that cannot be confused with the others.
    const auto first  = encode("first", 440.0, "First Link", kRate);
    const auto second = encode("second", 880.0, "Second Link", kRate);
    const auto third  = encode("third", 330.0, "Third Link", kRate / 2);
    if (first.empty() || second.empty() || third.empty()) {
        return {};
    }

    std::FILE* out = std::fopen(chained.string().c_str(), "wb");
    REQUIRE(out != nullptr);
    for (const auto& part : {first, second, third}) {
        const auto bytes = readBytes(part);
        std::fwrite(bytes.data(), 1, bytes.size(), out);
    }
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

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// A single-link encode, for the case that must NOT be expanded.
std::filesystem::path buildSingleStream() {
    const auto solo = fixtureDir() / "solo.oga";
    if (std::filesystem::exists(solo)) {
        return solo;
    }
    if (!xpcog::test::haveTool("flac")) {
        return {};
    }
    const auto        wav     = writeTone("solo.wav", 550.0, kRate);
    const std::string command = "flac --ogg -s -f --totally-silent -T \"TITLE=Solo\""
                                " -o \"" + solo.string() + "\" \"" + wav.string() +
                                "\"" + xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(solo)) {
        return {};
    }
    return solo;
}

/// Decodes a whole track through the registry, the way the scanner and the
/// engine reach it.
std::vector<float> decodeTrack(const Url& url, TrackProperties& props,
                               MetadataMap& tags) {
    auto opened = registry().open(url);
    REQUIRE(opened);
    props = opened.decoder->properties();
    tags  = opened.decoder->metadata();

    std::vector<float> pcm;
    AudioChunk         chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = pcm.size();
        pcm.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, samples));
    }
    return pcm;
}

}  // namespace

TEST_CASE("a chained Ogg FLAC file expands to one track per link", "[flac][chained]") {
    const auto path = buildChainedStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    // A file of chained links is a container: each link is a whole track with
    // its own headers, so listing them is what gives each a name, a length and a
    // seek bar of its own instead of one nameless run of everything.
    const Url        url     = Url::fromLocalPath(path);
    const std::vector<Url> tracks = registry().expandContainer(url);

    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0] == url.withFragment("0"));
    CHECK(tracks[1] == url.withFragment("1"));
    CHECK(tracks[2] == url.withFragment("2"));
}

TEST_CASE("an unchained Ogg FLAC file stays one track", "[flac][chained]") {
    const auto path = buildSingleStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    // The common case, and the one that must not pay for the feature: the chain
    // check is two reads, not a walk of every page in the file.
    const Url              url    = Url::fromLocalPath(path);
    const std::vector<Url> tracks = registry().expandContainer(url);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0] == url);

    TrackProperties props;
    MetadataMap     tags;
    const std::vector<float> pcm = decodeTrack(url, props, tags);
    // And it plays at all, which before this it did not: .oga was claimed only
    // by Vorbis, so an Ogg FLAC file under that name found no decoder.
    CHECK(tags.first("title") == "Solo");
    CHECK(static_cast<double>(pcm.size()) == Catch::Approx(kRate).margin(kRate * 0.05));
}

TEST_CASE("a chain with repeated serial numbers is left alone", "[flac][chained]") {
    if (!xpcog::test::haveTool("flac")) SKIP("the flac encoder is not available");

    // `cat x.oga x.oga` repeats the serial number rather than choosing a new
    // one, which RFC 3533 forbids: every logical bitstream in a physical one
    // must have a distinct serial. The cheap chain check compares the first
    // page's serial with the last page's, so it reads this as one link -- and
    // that is the honest answer, because the rule it breaks is the same one
    // libFLAC's demuxer uses to tell one link's pages from another's. There is
    // no correct reading of such a file available here.
    //
    // Pinned so the behaviour is a decision rather than an accident: it plays as
    // its first link instead of failing.
    const auto single = buildSingleStream();
    if (single.empty()) SKIP("the flac encoder is not available to build a fixture");

    const auto doubled = fixtureDir() / "doubled.oga";
    {
        const auto bytes = readBytes(single);
        std::FILE* out   = std::fopen(doubled.string().c_str(), "wb");
        REQUIRE(out != nullptr);
        std::fwrite(bytes.data(), 1, bytes.size(), out);
        std::fwrite(bytes.data(), 1, bytes.size(), out);
        std::fclose(out);
    }

    const Url              url    = Url::fromLocalPath(doubled);
    const std::vector<Url> tracks = registry().expandContainer(url);
    REQUIRE(tracks.size() == 1);
    CHECK(tracks[0] == url);

    TrackProperties props;
    MetadataMap     tags;
    const std::vector<float> pcm = decodeTrack(url, props, tags);
    CHECK(tags.first("title") == "Solo");
    CHECK(static_cast<double>(pcm.size()) == Catch::Approx(kRate).margin(kRate * 0.05));
}

TEST_CASE("each chain link decodes as its own track", "[flac][chained]") {
    const auto path = buildChainedStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    const Url url = Url::fromLocalPath(path);

    struct Expected {
        const char* fragment;
        const char* title;
        double      frequency;
        double      frames;
    };
    const Expected expected[] = {
        {"0", "First Link", 440.0, kRate},
        {"1", "Second Link", 880.0, kRate},
        {"2", "Third Link", 330.0, kRate / 2},
    };

    for (const Expected& want : expected) {
        INFO("link " << want.fragment);

        TrackProperties props;
        MetadataMap     tags;
        const std::vector<float> pcm =
            decodeTrack(url.withFragment(want.fragment), props, tags);

        // Its own tags, from its own Vorbis comment -- the thing decoding the
        // chain end to end could not give, since seeking into a link does not
        // re-deliver its headers.
        CHECK(tags.first("title") == want.title);

        // Its own length, from its own STREAMINFO, and a seek bar that means
        // something.
        CHECK(static_cast<double>(props.totalFrames) ==
              Catch::Approx(want.frames).margin(1.0));
        CHECK(props.seekable);

        // Its own audio, and only its own: reading past the link boundary would
        // append the next track's tone to this one.
        CHECK(static_cast<double>(pcm.size()) ==
              Catch::Approx(want.frames).margin(kRate * 0.05));
        CHECK(frequencyOf(std::span<const float>{pcm}) ==
              Catch::Approx(want.frequency).margin(15.0));
    }
}

TEST_CASE("seeking within a chain link stays inside it", "[flac][chained]") {
    const auto path = buildChainedStream();
    if (path.empty()) SKIP("the flac encoder is not available to build a fixture");

    // Seek offsets are the link's own, not the file's. Getting that wrong lands
    // in the wrong track entirely -- and silently, since every link is valid
    // FLAC and decodes cleanly wherever you land.
    const Url url = Url::fromLocalPath(path).withFragment("1");

    auto opened = registry().open(url);
    REQUIRE(opened);

    const auto target = static_cast<std::int64_t>(kRate / 2);
    REQUIRE(opened.decoder->seek(target) == target);

    std::vector<float> pcm;
    AudioChunk         chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = pcm.size();
        pcm.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{pcm}.subspan(offset, samples));
    }

    // Half of the second link and nothing after it.
    CHECK(static_cast<double>(pcm.size()) ==
          Catch::Approx(kRate / 2).margin(kRate * 0.05));
    CHECK(frequencyOf(std::span<const float>{pcm}) == Catch::Approx(880.0).margin(15.0));
}

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

    // Every link, not just the first. Stopping at the first boundary yields the
    // opening second and looks like a clean end of stream, which is why the bug
    // was invisible until a station outlived one song.
    const auto expected = kRate * 2.5;
    CHECK(static_cast<double>(pcm.size()) == Catch::Approx(expected).margin(kRate * 0.05));

    // And it is each link's own audio in turn, not more of the first.
    const auto                   second = static_cast<std::size_t>(kRate);
    const auto                   third  = static_cast<std::size_t>(kRate * 2);
    REQUIRE(pcm.size() > third);
    const std::span<const float> all{pcm};
    CHECK(frequencyOf(all.subspan(0, second)) == Catch::Approx(440.0).margin(15.0));
    CHECK(frequencyOf(all.subspan(second, second)) == Catch::Approx(880.0).margin(15.0));
    CHECK(frequencyOf(all.subspan(third)) == Catch::Approx(330.0).margin(15.0));
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

    // Once per boundary -- this is what renames the row on a station, and it is
    // announced from readAudio() rather than from the metadata callback because
    // a link's blocks arrive one at a time.
    CHECK(announcedTitles == std::vector<std::string>{"Second Link", "Third Link"});

    const MetadataMap tags = decoder.metadata();
    CHECK(tags.first("title") == "Third Link");

    // Replaced, not appended. Vorbis comments legitimately repeat -- a file may
    // carry several ARTIST lines -- so tags accumulate unless a link boundary
    // clears them, and a station's tag list would grow by a whole track every
    // few minutes with the first song's name still at the front.
    const auto* artists = std::get_if<std::vector<std::string>>(tags.find("artist"));
    REQUIRE(artists != nullptr);
    CHECK(artists->size() == 1);
    CHECK(artists->front() == "Third Link Artist");
}
