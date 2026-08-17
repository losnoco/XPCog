#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/AudioFormat.hpp"
#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace xpcog;

// --- Url ------------------------------------------------------------------

TEST_CASE("Url parses absolute URLs", "[url]") {
    const auto url = Url::parse("HTTP://example.com/Song.FLAC");
    REQUIRE(url.has_value());
    CHECK(url->scheme() == "http");  // lowercased
    CHECK(url->extension() == "flac");
    CHECK_FALSE(url->localPath().has_value());
}

TEST_CASE("Url keeps fragments, which carry cue and subsong indices", "[url]") {
    const auto url = Url::parse("file:///music/album.flac#12");
    REQUIRE(url.has_value());
    CHECK(url->fragment() == "12");
    // The fragment must not leak into the extension.
    CHECK(url->extension() == "flac");
    CHECK(url->withoutFragment().fragment().empty());
    CHECK(url->withFragment("3").fragment() == "3");
}

TEST_CASE("Url round-trips through toString", "[url]") {
    const std::string text = "file:///music/a%20b.flac#2";
    const auto        url  = Url::parse(text);
    REQUIRE(url.has_value());
    CHECK(url->toString() == text);
}

TEST_CASE("Url percent-decodes local paths", "[url]") {
    const auto url = Url::parse("file:///music/a%20b%26c.flac");
    REQUIRE(url.has_value());
    const auto path = url->localPath();
    REQUIRE(path.has_value());
    CHECK(path->filename().string() == "a b&c.flac");
}

TEST_CASE("Url::fromLocalPath survives spaces and non-ASCII", "[url]") {
    const auto url  = Url::fromLocalPath("/music/Sigur Rós/Untitled #1.flac");
    const auto path = url.localPath();
    REQUIRE(path.has_value());
    CHECK(path->filename().string() == "Untitled #1.flac");
    // '#' must be encoded, or it would be misread as a fragment.
    CHECK(url.fragment().empty());
}

TEST_CASE("Url rejects bare paths and Windows drive letters", "[url]") {
    CHECK_FALSE(Url::parse("/music/a.flac").has_value());
    CHECK_FALSE(Url::parse("relative.flac").has_value());
    // A single-letter scheme is a drive letter, not a URL.
    CHECK_FALSE(Url::parse("C:\\music\\a.flac").has_value());
}

// --- AudioFormat ----------------------------------------------------------

TEST_CASE("guessChannelConfig matches Cog's table", "[format]") {
    CHECK(guessChannelConfig(1) == kConfigMono);
    CHECK(guessChannelConfig(2) == kConfigStereo);
    CHECK(guessChannelConfig(6) == kConfig5Point1);
    CHECK(guessChannelConfig(8) == kConfig7Point1);
    CHECK(guessChannelConfig(0) == 0);

    // Counts with no table entry fall back to a low-bits mask.
    CHECK(countChannels(guessChannelConfig(9)) == 9);
    // 32 channels must not shift by 32, which would be UB.
    CHECK(countChannels(guessChannelConfig(32)) == 32);
}

TEST_CASE("channel index helpers round-trip", "[format]") {
    constexpr std::uint32_t config = kConfig5Point1;
    for (std::uint32_t i = 0; i < countChannels(config); ++i) {
        const std::uint32_t flag = extractChannelFlag(i, config);
        REQUIRE(flag != 0);
        CHECK(channelIndexFromConfig(config, flag) == i);
    }
    CHECK(findChannelIndex(kChannelLFE) == 3);
}

TEST_CASE("AudioFormat computes frame sizes", "[format]") {
    AudioFormat fmt;
    fmt.channels = 2;
    fmt.format   = SampleFormat::S24;
    CHECK(fmt.bytesPerSample() == 3);
    CHECK(fmt.bytesPerFrame() == 6);
    CHECK_FALSE(fmt.isFloat());
    CHECK_FALSE(fmt.valid());  // no sample rate yet

    fmt.sampleRate = 44100.0;
    CHECK(fmt.valid());
}

// --- AudioChunk -----------------------------------------------------------

namespace {

AudioChunk makeChunk(std::size_t frames) {
    AudioFormat fmt;
    fmt.sampleRate = 44100.0;
    fmt.channels   = 2;
    fmt.format     = SampleFormat::S16;

    AudioChunk chunk;
    chunk.setFormat(fmt);
    std::byte* data = chunk.allocFrames(frames);
    for (std::size_t i = 0; i < frames * fmt.bytesPerFrame(); ++i) {
        data[i] = static_cast<std::byte>(i & 0xFF);
    }
    return chunk;
}

}  // namespace

TEST_CASE("AudioChunk reports frames and duration", "[chunk]") {
    const AudioChunk chunk = makeChunk(44100);
    CHECK(chunk.frameCount() == 44100);
    CHECK(chunk.duration() == Catch::Approx(1.0));
}

TEST_CASE("AudioChunk::removeFrames splits and advances the timestamp", "[chunk]") {
    AudioChunk chunk = makeChunk(1000);
    chunk.resetForward = true;

    AudioChunk taken;
    chunk.removeFrames(400, taken);

    CHECK(taken.frameCount() == 400);
    CHECK(chunk.frameCount() == 600);

    // The reset applies to the leading edge, so it travels with the removed part.
    CHECK(taken.resetForward);
    CHECK_FALSE(chunk.resetForward);

    // What remains starts 400 frames later in the stream.
    CHECK(chunk.streamTimestamp == Catch::Approx(400.0 / 44100.0));

    // The split must preserve sample data exactly.
    CHECK(static_cast<int>(taken.bytes()[0]) == 0);
    CHECK(static_cast<int>(chunk.bytes()[0]) == static_cast<int>(400 * 4 & 0xFF));
}

TEST_CASE("AudioChunk::removeFrames clamps to what is available", "[chunk]") {
    AudioChunk chunk = makeChunk(10);
    AudioChunk taken;
    chunk.removeFrames(999, taken);
    CHECK(taken.frameCount() == 10);
    CHECK(chunk.empty());
}

TEST_CASE("AudioChunk::clear keeps the format for reuse", "[chunk]") {
    AudioChunk chunk = makeChunk(100);
    chunk.clear();
    CHECK(chunk.empty());
    CHECK(chunk.frameCount() == 0);
    CHECK(chunk.format().channels == 2);  // still usable without re-setting
}

// --- MetadataMap ----------------------------------------------------------

TEST_CASE("MetadataMap keeps repeated tags as multiple values", "[metadata]") {
    MetadataMap map;
    map.add("ARTIST", "First");
    map.add("artist", "Second");

    CHECK(map.size() == 1);  // one key, two values
    CHECK(map.first("artist") == "First");
    CHECK(map.joined("artist", "; ") == "First; Second");
}

TEST_CASE("MetadataMap normalizes keys the way Cog does", "[metadata]") {
    // '.' becomes U+2024, because Cocoa bindings treat '.' as a key path.
    CHECK(MetadataMap::normalizeKey("WM.Foo") == "wm\u2024foo");
    CHECK(MetadataMap::normalizeKey("TiTlE") == "title");
}

TEST_CASE("MetadataMap stores binary album art distinctly", "[metadata]") {
    MetadataMap map;
    map.setBytes("albumart", {std::byte{1}, std::byte{2}});

    CHECK(map.bytes("albumart") != nullptr);
    CHECK(map.bytes("albumart")->size() == 2);
    // A bytes value has no string form.
    CHECK(map.first("albumart").empty());
    CHECK(map.joined("albumart").empty());
}

TEST_CASE("MetadataMap::mergeFrom overwrites on collision", "[metadata]") {
    MetadataMap base;
    base.set("title", "Old");
    base.set("album", "Kept");

    MetadataMap overlay;
    overlay.set("title", "New");

    base.mergeFrom(overlay);
    CHECK(base.first("title") == "New");
    CHECK(base.first("album") == "Kept");
    CHECK(base.size() == 2);
}

TEST_CASE("MetadataMap preserves insertion order", "[metadata]") {
    MetadataMap map;
    map.set("zebra", "1");
    map.set("alpha", "2");
    map.set("middle", "3");

    std::vector<std::string> keys;
    for (const auto& entry : map) {
        keys.push_back(entry.key);
    }
    CHECK(keys == std::vector<std::string>{"zebra", "alpha", "middle"});
}
