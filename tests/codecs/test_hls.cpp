// HLS: the manifest parser, the memory source between the fetcher and the
// decoder, and the whole path end to end.
//
// The first two are where the bugs live and neither fails visibly. A URI that
// resolves to the wrong place looks like a server error; a memory source that
// blocks when it should return short looks like a slow network. Both are pure
// enough to test by handing them strings and bytes, which is why they are
// separate classes at all.
//
// The end-to-end case needs a real segmented stream, so ffmpeg makes one and the
// test skips when it is not installed -- the same rule the conformance harness
// uses. It is the only check that the pieces are wired to each other rather than
// merely correct in isolation.

#include "hls/HlsMemorySource.hpp"
#include "hls/HlsPlaylist.hpp"
#include "hls/HlsSegmentManager.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;
using xpcog::codecs::HlsMemorySource;
using xpcog::codecs::HlsSegmentManager;
using xpcog::codecs::parseAttributeList;
using xpcog::codecs::parseHlsPlaylist;
using xpcog::codecs::resolveHlsUri;

namespace {

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

Url base(const char* text) { return *Url::parse(text); }

std::string resolved(const char* reference, const char* baseText) {
    const auto url = resolveHlsUri(reference, base(baseText));
    return url ? url->toString() : std::string{"<none>"};
}

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::string valueOf(const std::vector<std::pair<std::string, std::string>>& attributes,
                    std::string_view key) {
    for (const auto& [name, value] : attributes) {
        if (name == key) {
            return value;
        }
    }
    return "<none>";
}

}  // namespace

// ---------------------------------------------------------------------------
// URI resolution
// ---------------------------------------------------------------------------

TEST_CASE("HLS resolves segment URIs against the manifest", "[hls]") {
    const char* manifest = "https://cdn.example.com/live/hi/index.m3u8";

    SECTION("relative to the manifest's directory") {
        CHECK(resolved("segment0.ts", manifest) ==
              "https://cdn.example.com/live/hi/segment0.ts");
    }
    SECTION("absolute path replaces the whole path") {
        CHECK(resolved("/other/seg.aac", manifest) ==
              "https://cdn.example.com/other/seg.aac");
    }
    SECTION("protocol-relative replaces the host") {
        CHECK(resolved("//edge2.example.com/a/b.ts", manifest) ==
              "https://edge2.example.com/a/b.ts");
    }
    SECTION("absolute passes through") {
        CHECK(resolved("http://elsewhere.test/x.ts", manifest) ==
              "http://elsewhere.test/x.ts");
    }
    SECTION("dot segments are removed") {
        // Left in place the server sees a literal ".." and answers 404 or, worse,
        // something else.
        CHECK(resolved("../lo/seg1.ts", manifest) ==
              "https://cdn.example.com/live/lo/seg1.ts");
        CHECK(resolved("./seg2.ts", manifest) ==
              "https://cdn.example.com/live/hi/seg2.ts");
        CHECK(resolved("../../seg3.ts", manifest) ==
              "https://cdn.example.com/seg3.ts");
    }
    SECTION("the reference's own query survives") {
        CHECK(resolved("seg.ts?token=abc", manifest) ==
              "https://cdn.example.com/live/hi/seg.ts?token=abc");
    }
    SECTION("the manifest's query does not leak onto the segment") {
        // A signed manifest URL carries a token the segments have their own copy
        // of; inheriting it produces a URL the origin has never issued.
        CHECK(resolved("seg.ts", "https://cdn.example.com/live/index.m3u8?tok=1") ==
              "https://cdn.example.com/live/seg.ts");
    }
    SECTION("a file manifest resolves to file segments") {
        CHECK(resolved("seg0.ts", "file:///music/stream/index.m3u8") ==
              "file:///music/stream/seg0.ts");
    }
}

// ---------------------------------------------------------------------------
// Attribute lists
// ---------------------------------------------------------------------------

TEST_CASE("HLS attribute lists split at top-level commas only", "[hls]") {
    // The comma inside CODECS is the whole reason this is not a split(',').
    const auto attributes = parseAttributeList(
        R"(BANDWIDTH=1280000,CODECS="mp4a.40.2,avc1.42E01E",RESOLUTION=640x360)");

    CHECK(valueOf(attributes, "BANDWIDTH") == "1280000");
    CHECK(valueOf(attributes, "CODECS") == "mp4a.40.2,avc1.42E01E");
    CHECK(valueOf(attributes, "RESOLUTION") == "640x360");
    CHECK(attributes.size() == 3);
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------

TEST_CASE("HLS parses a VOD media playlist", "[hls]") {
    const auto playlist = parseHlsPlaylist(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:10\n"
        "#EXT-X-MEDIA-SEQUENCE:7\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:9.009,First\n"
        "a.aac\n"
        "#EXTINF:8.5,\n"
        "b.aac\n"
        "#EXT-X-ENDLIST\n",
        base("https://example.test/vod/index.m3u8"));

    REQUIRE(playlist);
    CHECK_FALSE(playlist->isMaster);
    CHECK_FALSE(playlist->isLive);
    CHECK(playlist->hasEndList);
    CHECK(playlist->version == 3);
    CHECK(playlist->targetDuration == 10);
    REQUIRE(playlist->segments.size() == 2);

    CHECK(playlist->segments[0].url.toString() == "https://example.test/vod/a.aac");
    CHECK(playlist->segments[0].duration == Catch::Approx(9.009));
    CHECK(playlist->segments[0].title == "First");
    // Numbered from EXT-X-MEDIA-SEQUENCE, which is what identifies a segment
    // across a live refresh.
    CHECK(playlist->segments[0].sequenceNumber == 7);
    CHECK(playlist->segments[1].sequenceNumber == 8);
    CHECK(playlist->totalDuration() == Catch::Approx(17.509));
}

TEST_CASE("HLS treats a playlist without an end list as live", "[hls]") {
    const auto playlist = parseHlsPlaylist("#EXTM3U\n"
                                           "#EXT-X-TARGETDURATION:6\n"
                                           "#EXTINF:6,\n"
                                           "s.ts\n",
                                           base("https://example.test/l/i.m3u8"));
    REQUIRE(playlist);
    CHECK(playlist->isLive);
    // A live playlist has no length: the window is not the programme.
    CHECK(playlist->totalDuration() == 0.0);
}

TEST_CASE("HLS keeps an EVENT playlist live until it ends", "[hls]") {
    // EVENT only ever appends -- but it does append, so it is not finite yet.
    const auto playlist = parseHlsPlaylist("#EXTM3U\n"
                                           "#EXT-X-TARGETDURATION:6\n"
                                           "#EXT-X-PLAYLIST-TYPE:EVENT\n"
                                           "#EXTINF:6,\n"
                                           "s.ts\n",
                                           base("https://example.test/e/i.m3u8"));
    REQUIRE(playlist);
    CHECK(playlist->isLive);
}

TEST_CASE("HLS picks the highest-bandwidth variant of a master", "[hls]") {
    const auto playlist = parseHlsPlaylist(
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.5\"\n"
        "lo/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=256000,CODECS=\"mp4a.40.2\"\n"
        "hi/index.m3u8\n",
        base("https://example.test/master.m3u8"));

    REQUIRE(playlist);
    CHECK(playlist->isMaster);
    REQUIRE(playlist->variants.size() == 2);

    const auto* best = playlist->bestVariant();
    REQUIRE(best != nullptr);
    CHECK(best->bandwidth == 256000);
    CHECK(best->codecs == "mp4a.40.2");
    CHECK(best->url.toString() == "https://example.test/hi/index.m3u8");
}

TEST_CASE("HLS applies EXT-X-KEY forward and stops at METHOD=NONE", "[hls]") {
    // The tag governs every following segment, not just the next one -- getting
    // that wrong means a stream is refused one segment late, after the decoder
    // has already been handed ciphertext.
    const auto playlist = parseHlsPlaylist(
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x0123456789ABCDEF0123456789ABCDEF\n"
        "#EXTINF:4,\n"
        "a.ts\n"
        "#EXTINF:4,\n"
        "b.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:4,\n"
        "c.ts\n"
        "#EXT-X-ENDLIST\n",
        base("https://example.test/k/i.m3u8"));

    REQUIRE(playlist);
    REQUIRE(playlist->segments.size() == 3);
    CHECK(playlist->segments[0].encrypted);
    CHECK(playlist->segments[1].encrypted);
    CHECK_FALSE(playlist->segments[2].encrypted);
    CHECK(playlist->segments[0].encryptionMethod == "AES-128");
    CHECK(playlist->segments[0].encryptionKeyUrl.toString() ==
          "https://example.test/k/key.bin");
    CHECK(playlist->segments[0].iv.size() == 16);
}

TEST_CASE("HLS records EXT-X-MAP and discontinuities", "[hls]") {
    const auto playlist = parseHlsPlaylist("#EXTM3U\n"
                                           "#EXT-X-TARGETDURATION:4\n"
                                           "#EXT-X-MAP:URI=\"init.mp4\"\n"
                                           "#EXTINF:4,\n"
                                           "a.m4s\n"
                                           "#EXT-X-DISCONTINUITY\n"
                                           "#EXTINF:4,\n"
                                           "b.m4s\n"
                                           "#EXT-X-ENDLIST\n",
                                           base("https://example.test/f/i.m3u8"));

    REQUIRE(playlist);
    REQUIRE(playlist->segments.size() == 2);
    CHECK(playlist->segments[0].mapUrl.toString() == "https://example.test/f/init.mp4");
    CHECK(playlist->segments[1].mapUrl.toString() == "https://example.test/f/init.mp4");
    CHECK_FALSE(playlist->segments[0].discontinuity);
    CHECK(playlist->segments[1].discontinuity);
}

TEST_CASE("HLS refuses things that are not manifests", "[hls]") {
    const Url url = base("https://example.test/x.m3u");

    // An ordinary M3U wears the same extension and can even carry #EXTINF. What
    // it cannot carry is the tag RFC 8216 makes mandatory, so that is the test --
    // claiming this one would turn a track list into a single unplayable stream.
    CHECK_FALSE(parseHlsPlaylist("#EXTM3U\n#EXTINF:123,Artist - Title\nsong.mp3\n", url));
    CHECK_FALSE(parseHlsPlaylist("song.mp3\nother.flac\n", url));
    CHECK_FALSE(parseHlsPlaylist("", url));
    // Header present and mandatory tag present, but nothing to play.
    CHECK_FALSE(parseHlsPlaylist("#EXTM3U\n#EXT-X-TARGETDURATION:10\n", url));
    // A manifest must lead with #EXTM3U.
    CHECK_FALSE(parseHlsPlaylist("#EXT-X-TARGETDURATION:10\n#EXTM3U\n#EXTINF:4,\na.ts\n",
                                 url));
}

// ---------------------------------------------------------------------------
// The memory source
// ---------------------------------------------------------------------------

TEST_CASE("HLS memory source concatenates segments", "[hls]") {
    HlsMemorySource source{base("https://example.test/stream.ts"), "video/mp2t"};
    source.append(bytesOf("abcde"));
    source.append(bytesOf("fghij"));
    source.markEndOfStream();

    char         buffer[16] = {};
    std::int64_t got        = source.read(buffer, 5);
    CHECK(got == 5);
    CHECK(std::string(buffer, 5) == "abcde");

    // A read spanning the boundary returns what the first chunk had left rather
    // than blocking for the rest: a short read is legal and the bytes are there.
    got = source.read(buffer, 16);
    CHECK(got == 5);
    CHECK(std::string(buffer, 5) == "fghij");

    CHECK(source.read(buffer, 16) == 0);  // end of stream
    CHECK(source.tell() == 10);
    CHECK_FALSE(source.seekable());
    CHECK_FALSE(source.seek(0, SEEK_SET));
}

TEST_CASE("HLS memory source blocks until a segment arrives", "[hls]") {
    HlsMemorySource source{base("https://example.test/stream.ts"), "video/mp2t"};

    std::thread producer([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        source.append(bytesOf("late"));
        source.markEndOfStream();
    });

    // The wait is the point: without it the decoder would see a premature EOF
    // every time the fetcher fell one round trip behind.
    char       buffer[8] = {};
    const auto got       = source.read(buffer, 4);
    producer.join();

    CHECK(got == 4);
    CHECK(std::string(buffer, 4) == "late");
}

TEST_CASE("HLS memory source unblocks a reader on close", "[hls]") {
    // interrupt() has to reach a decoder parked inside read(), or stopping
    // playback on a stalled stream hangs the decode thread until the server
    // answers -- which for a dead origin is never.
    HlsMemorySource source{base("https://example.test/stream.ts"), "video/mp2t"};

    std::thread stopper([&source] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        source.interrupt();
    });

    char       buffer[8] = {};
    const auto got       = source.read(buffer, 4);
    stopper.join();
    CHECK(got == 0);
}

TEST_CASE("HLS memory source reset drops the buffered future", "[hls]") {
    // What a seek does: the queued segments are for the wrong part of the
    // programme and the position restarts from the new one.
    HlsMemorySource source{base("https://example.test/stream.ts"), "video/mp2t"};
    source.append(bytesOf("stale"));

    char buffer[8] = {};
    REQUIRE(source.read(buffer, 2) == 2);
    CHECK(source.bufferedSegments() == 1);

    source.reset();
    CHECK(source.bufferedSegments() == 0);
    CHECK(source.tell() == 0);

    source.append(bytesOf("fresh"));
    source.markEndOfStream();
    REQUIRE(source.read(buffer, 5) == 5);
    CHECK(std::string(buffer, 5) == "fresh");
}

// ---------------------------------------------------------------------------
// The fetcher
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path liveDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-hls-live";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
}

/// Polls `predicate` until it holds or the deadline passes. Wall-clock waiting is
/// unavoidable here: the thing under test is a background thread on a one-second
/// refresh cadence, and the alternative is asserting on a timer.
template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return predicate();
}

}  // namespace

TEST_CASE("HLS live refresh appends only segments it has not seen", "[hls]") {
    // The one piece of the fetcher that cannot be reasoned about from the
    // manifest alone: a live playlist repeats the segments still in its window
    // on every reload, so re-appending them is the natural bug -- and it does not
    // fail, it just plays the last few seconds over and over.
    const auto directory = liveDir();
    writeFile(directory / "seg0.bin", "AAAA");
    writeFile(directory / "seg1.bin", "BBBB");

    const auto manifestPath = directory / "live.m3u8";
    writeFile(manifestPath, "#EXTM3U\n"
                            "#EXT-X-TARGETDURATION:1\n"
                            "#EXT-X-MEDIA-SEQUENCE:0\n"
                            "#EXTINF:1,\n"
                            "seg0.bin\n");

    const Url  manifestUrl = Url::fromLocalPath(manifestPath);
    const auto playlist    = parseHlsPlaylist(
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:1,\n"
        "seg0.bin\n",
        manifestUrl);
    REQUIRE(playlist);
    REQUIRE(playlist->isLive);

    HlsMemorySource   memory{manifestUrl, "application/octet-stream"};
    HlsSegmentManager manager{*playlist, registry(), memory};
    manager.start(0);

    REQUIRE(waitUntil([&] { return memory.bufferedSegments() == 1; },
                      std::chrono::seconds{5}));

    // The publisher extends the stream and declares it finished. seg0 is still
    // listed, as a real window would list it.
    writeFile(manifestPath, "#EXTM3U\n"
                            "#EXT-X-TARGETDURATION:1\n"
                            "#EXT-X-MEDIA-SEQUENCE:0\n"
                            "#EXTINF:1,\n"
                            "seg0.bin\n"
                            "#EXTINF:1,\n"
                            "seg1.bin\n"
                            "#EXT-X-ENDLIST\n");

    REQUIRE(waitUntil([&] { return manager.segments().size() == 2; },
                      std::chrono::seconds{5}));
    // ENDLIST adopted, so the fetcher stops reloading and lets the stream drain.
    CHECK_FALSE(manager.isLive());

    REQUIRE(waitUntil([&] { return memory.bufferedSegments() == 2; },
                      std::chrono::seconds{5}));
    manager.stop();

    // Exactly one copy of each: re-appending seg0 would make this "AAAAAAAABBBB".
    std::string received;
    char        buffer[64] = {};
    std::int64_t got       = 0;
    while ((got = memory.read(buffer, sizeof(buffer))) > 0) {
        received.append(buffer, static_cast<std::size_t>(got));
    }
    CHECK(received == "AAAABBBB");
}

TEST_CASE("HLS fetcher stops promptly while a download is pending", "[hls]") {
    // stop() joins the fetch thread, so anything it can block on has to be
    // interruptible -- otherwise closing a stalled stream hangs playback.
    const auto directory = liveDir();
    writeFile(directory / "only.bin", "XYZ");

    const auto manifestPath = directory / "vod.m3u8";
    writeFile(manifestPath, "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nonly.bin\n"
                            "#EXT-X-ENDLIST\n");

    const Url  manifestUrl = Url::fromLocalPath(manifestPath);
    const auto playlist    = parseHlsPlaylist(
        "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nonly.bin\n#EXT-X-ENDLIST\n",
        manifestUrl);
    REQUIRE(playlist);

    HlsMemorySource   memory{manifestUrl, "application/octet-stream"};
    HlsSegmentManager manager{*playlist, registry(), memory};
    manager.start(0);

    const auto before = std::chrono::steady_clock::now();
    manager.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds{2});
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kLeftFreq   = 440.0;
constexpr double kRightFreq  = 660.0;
constexpr double kLeftAmp    = 0.67;
constexpr double kRightAmp   = 0.55;
constexpr int    kFrames     = 44100 * 4;  // four seconds, so there are segments

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-hls-tests";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

std::filesystem::path referenceWav() {
    const auto out = fixtureDir() / "reference.wav";
    if (std::filesystem::exists(out)) {
        return out;
    }

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
    REQUIRE(f != nullptr);
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
}

/// A real VOD stream: AAC in MPEG-TS segments with a manifest beside them, which
/// is what an origin actually serves. Empty when ffmpeg cannot make one.
std::filesystem::path buildStream() {
    const auto manifest = fixtureDir() / "index.m3u8";
    if (std::filesystem::exists(manifest)) {
        return manifest;
    }
    if (!xpcog::test::haveTool("ffmpeg")) {
        return {};
    }

    const std::string command =
        "ffmpeg -y -loglevel error -i \"" + referenceWav().string() +
        "\" -c:a aac -b:a 192k -f hls -hls_time 1 -hls_list_size 0"
        " -hls_playlist_type vod -hls_segment_filename \"" +
        (fixtureDir() / "seg%d.ts").string() + "\" \"" + manifest.string() + "\"" +
        xpcog::test::kSilenceStderr;

    if (std::system(command.c_str()) != 0 || !std::filesystem::exists(manifest)) {
        return {};
    }
    return manifest;
}

/// Zero-crossing frequency and RMS per channel over the steady middle, so the
/// codec's leading padding does not skew either.
struct ChannelStats {
    double rms       = 0.0;
    double frequency = 0.0;
};

std::vector<ChannelStats> analyse(const std::vector<float>& interleaved,
                                  std::uint32_t channels, double rate) {
    std::vector<ChannelStats> stats(channels);
    const std::size_t         frames = interleaved.size() / channels;
    const std::size_t         begin  = frames / 4;
    const std::size_t         end    = frames * 3 / 4;

    for (std::uint32_t c = 0; c < channels; ++c) {
        double      sumSquares = 0.0;
        std::size_t crossings  = 0;
        float       previous   = 0.0F;
        for (std::size_t i = begin; i < end; ++i) {
            const float v = interleaved[i * channels + c];
            sumSquares += static_cast<double>(v) * static_cast<double>(v);
            if (i > begin && ((previous < 0.0F) != (v < 0.0F))) {
                ++crossings;
            }
            previous = v;
        }
        const auto count   = static_cast<double>(end - begin);
        stats[c].rms       = std::sqrt(sumSquares / count);
        stats[c].frequency = static_cast<double>(crossings) * rate / (2.0 * count);
    }
    return stats;
}

std::vector<float> drain(IDecoder& decoder) {
    std::vector<float> out;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = out.size();
        out.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{out}.subspan(offset, samples));
    }
    return out;
}

const bool kHaveHls = [] {
    const auto extensions = registry().allExtensions();
    return std::find(extensions.begin(), extensions.end(), "m3u8") != extensions.end();
}();

}  // namespace

TEST_CASE("an HLS stream decodes to the signal it was built from", "[hls][conformance]") {
    if (!kHaveHls) SKIP("HLS is not built into this configuration");

    const auto manifest = buildStream();
    if (manifest.empty()) SKIP("ffmpeg not available to build an HLS fixture");

    // The manifest is not a track list: expansion must hand it back unchanged so
    // the decoder layer gets it. Returning nothing here is how it used to vanish
    // from the playlist entirely.
    const Url  url     = Url::fromLocalPath(manifest);
    const auto entries = registry().expandContainer(url);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0] == url);

    auto opened = registry().open(url);
    REQUIRE(opened);

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.format.channels == 2);
    CHECK(props.format.sampleRate == Catch::Approx(kSampleRate));
    // A VOD manifest has a length, summed from the segment durations it declares.
    CHECK(props.seekable);
    CHECK(static_cast<double>(props.totalFrames) / props.format.sampleRate ==
          Catch::Approx(4.0).margin(0.3));

    const std::vector<float> pcm = drain(*opened.decoder);
    REQUIRE_FALSE(pcm.empty());

    // Every segment has to have been fetched and concatenated: stopping after the
    // first would still decode cleanly, just a quarter as long.
    const double seconds = static_cast<double>(pcm.size() / 2) / props.format.sampleRate;
    CHECK(seconds == Catch::Approx(4.0).margin(0.3));

    const auto stats = analyse(pcm, 2, props.format.sampleRate);
    // Asymmetric on purpose: swapped, duplicated or silent channels all fail here,
    // and a segment stitched in at the wrong offset shifts the frequencies.
    CHECK(stats[0].frequency == Catch::Approx(kLeftFreq).margin(15.0));
    CHECK(stats[1].frequency == Catch::Approx(kRightFreq).margin(15.0));
    CHECK(stats[0].rms == Catch::Approx(kLeftAmp / std::sqrt(2.0)).margin(0.05));
    CHECK(stats[1].rms == Catch::Approx(kRightAmp / std::sqrt(2.0)).margin(0.05));
}

TEST_CASE("seeking an HLS stream lands on the right audio", "[hls][conformance]") {
    if (!kHaveHls) SKIP("HLS is not built into this configuration");

    const auto manifest = buildStream();
    if (manifest.empty()) SKIP("ffmpeg not available to build an HLS fixture");

    const Url url = Url::fromLocalPath(manifest);

    std::vector<float> whole;
    double             rate     = 0.0;
    std::uint32_t      channels = 0;
    {
        auto opened = registry().open(url);
        REQUIRE(opened);
        rate     = opened.decoder->properties().format.sampleRate;
        channels = opened.decoder->properties().format.channels;
        whole    = drain(*opened.decoder);
    }
    REQUIRE(rate > 0.0);
    REQUIRE(channels == 2);

    // Two seconds in: past the first segment boundary, so this exercises the
    // fetch-a-different-segment-and-reopen path rather than a no-op.
    const auto target = static_cast<std::int64_t>(rate * 2.0);
    REQUIRE(static_cast<std::size_t>(target) * channels < whole.size());

    auto opened = registry().open(url);
    REQUIRE(opened);
    REQUIRE(opened.decoder->seek(target) == target);

    const std::vector<float> after = drain(*opened.decoder);
    REQUIRE(after.size() > channels * 4096);

    // Content, not just a return code: a seek that reports the right frame and
    // delivers the wrong samples is the failure this is for, and landing on the
    // segment boundary instead of the requested frame would be off by up to a
    // whole second.
    const std::size_t offset  = static_cast<std::size_t>(target) * channels;
    const std::size_t compare = std::min<std::size_t>(after.size(), whole.size() - offset);
    REQUIRE(compare > channels * 4096);

    const auto worstDifference = [&](std::size_t from) {
        double worst = 0.0;
        for (std::size_t i = 0; i < compare; ++i) {
            worst = std::max(worst,
                             static_cast<double>(std::fabs(after[i] - whole[from + i])));
        }
        return worst;
    };

    // Not sample-exact, and cannot be: the landing point is computed from the
    // durations the manifest declares, which are rounded, so the result can sit
    // a sample or two out. One sample of shift on the 660 Hz channel is already
    // 0.55 * 2*pi*660/44100 = 0.05, which is what the tolerance is scaled to.
    CHECK(worstDifference(offset) < 0.15);

    // The tolerance only means something if a wrong landing point fails it. Half
    // a second out is what "seeked to the segment boundary and forgot the
    // remainder" would look like, and it has to be visibly worse.
    const auto strayOffset = offset + static_cast<std::size_t>(rate / 2.0) * channels;
    if (strayOffset + compare <= whole.size()) {
        CHECK(worstDifference(strayOffset) > 0.3);
    }
}
