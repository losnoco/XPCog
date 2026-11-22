// MP3 chapters in the decoder that plays them when FFmpeg is not built.
//
// The universal cases run in every build: a chaptered file still plays whole
// without a fragment, and the sibling ID3v2 parser reads the same chapters
// TagLib does. The fragment-acceptance cases compile only in the no-FFmpeg
// build (XPCOG_WITH_FFMPEG=0, the linux-minimal preset): with FFmpeg built,
// minimp3 refuses fragments and FFmpeg plays the spans -- covered by
// test_chapters_ffmpeg.cpp -- so asserting acceptance here would assert the
// wrong build's behaviour.

#include "common/Id3v2.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"
#include "../TestSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

namespace fs = std::filesystem;

constexpr int kRate = 44100;

fs::path fixtureDir() {
    static const fs::path dir = [] {
        const fs::path path = fs::temp_directory_path() / "xpcog-mp3-chapter-tests";
        std::error_code ignored;
        fs::remove_all(path, ignored);
        fs::create_directories(path, ignored);
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

/// A second of silence, encoded by LAME through ffmpeg: the Xing/Info header
/// carries the gapless numbers the chapter-0 case leans on.
fs::path makeMp3(const std::string& name) {
    const fs::path wav = fixtureDir() / (name + ".wav");
    const fs::path mp3 = fixtureDir() / (name + ".mp3");
    {
        std::ofstream out{wav, std::ios::binary};
        const std::vector<std::uint8_t> silence(kRate * 4, 0);
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(silence.size());
        const auto write32 = [&out](std::uint32_t v) {
            const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                               static_cast<char>((v >> 16) & 0xFF),
                               static_cast<char>((v >> 24) & 0xFF)};
            out.write(b, 4);
        };
        const auto write16 = [&out](std::uint16_t v) {
            const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
            out.write(b, 2);
        };
        out.write("RIFF", 4);
        write32(36 + dataBytes);
        out.write("WAVEfmt ", 8);
        write32(16);
        write16(1);
        write16(2);
        write32(kRate);
        write32(kRate * 4);
        write16(4);
        write16(16);
        out.write("data", 4);
        write32(dataBytes);
        out.write(reinterpret_cast<const char*>(silence.data()), dataBytes);
    }
    std::error_code ignored;
    fs::remove(mp3, ignored);
    const std::string command = "ffmpeg -loglevel error -y -i \"" + wav.string() +
                                "\" -codec:a libmp3lame -b:a 32k \"" + mp3.string() + "\"" +
                                xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !fs::exists(mp3)) {
        return {};
    }
    return mp3;
}

Chapter chapterAt(std::uint64_t startMs, std::string title) {
    Chapter chapter;
    chapter.startMs = startMs;
    chapter.title   = std::move(title);
    return chapter;
}

ChapterList threeChapters() {
    return ChapterList{chapterAt(0, "One"), chapterAt(300, "Two"), chapterAt(600, "Three")};
}

bool writeTagsAndChapters(const fs::path& mp3, const MetadataMap& tags,
                          const ChapterList& chapters) {
    MetadataWriteOptions options;
    return registry().writeMetadata(Url::fromLocalPath(mp3), tags, chapters, options);
}

std::vector<std::uint8_t> fileBytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

#if !XPCOG_WITH_FFMPEG

/// The whole file decoded sequentially: the reference the chapter spans are
/// measured against, in the ex layer's gapless-adjusted sample space.
std::vector<float> decodeAll(IDecoder& decoder) {
    std::vector<float> out;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const auto* data = reinterpret_cast<const float*>(chunk.bytes().data());
        const std::size_t got = chunk.frameCount() * chunk.format().channels;
        out.insert(out.end(), data, data + got);
    }
    return out;
}

std::vector<float> readFrames(IDecoder& decoder, std::size_t frames) {
    std::vector<float> out;
    AudioChunk         chunk;
    while (out.size() < frames && decoder.readAudio(chunk)) {
        const auto* data = reinterpret_cast<const float*>(chunk.bytes().data());
        const std::size_t got = chunk.frameCount() * chunk.format().channels;
        out.insert(out.end(), data, data + got);
    }
    // Chunks arrive whole; the window is the request, not the last chunk's end.
    if (out.size() > frames) {
        out.resize(frames);
    }
    return out;
}

float worstDifference(const std::vector<float>& a, const std::vector<float>& b,
                      std::size_t bOffset, std::size_t count) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < count && i < a.size() && bOffset + i < b.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[bOffset + i]));
    }
    return worst;
}

/// A registry whose only source is the unseekable one, as test_mp3.cpp has.
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
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

Url streamUrl(const fs::path& path) {
    std::string text = Url::fromLocalPath(path).toString();
    text.replace(0, 4, "teststream");
    return *Url::parse(text);
}

#endif  // !XPCOG_WITH_FFMPEG

}  // namespace

TEST_CASE("a chaptered MP3 plays whole without a fragment", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("whole");
    REQUIRE_FALSE(mp3.empty());
    MetadataMap tags;
    tags.set("title", "The Book");
    REQUIRE(writeTagsAndChapters(mp3, tags, threeChapters()));

    const auto opened = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(opened);
    CHECK(opened.decoder->properties().codec == "MP3");
    // A second of audio, gapless-trimmed to the sample.
    CHECK(std::abs(opened.decoder->properties().totalFrames - kRate) <= 1);
    AudioChunk chunk;
    CHECK(opened.decoder->readAudio(chunk));
    CHECK_FALSE(chunk.empty());
}

TEST_CASE("the sibling parser and TagLib read the same chapters", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("agreement");
    REQUIRE_FALSE(mp3.empty());
    REQUIRE(writeTagsAndChapters(mp3, {}, threeChapters()));

    ChapterList sibling;
    const auto  bytes = fileBytes(mp3);
    REQUIRE(codecs::parseId3v2Chapters(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(bytes.data()),
                                   bytes.size()},
        sibling));
    const ChapterList taglib = registry().readChapters(Url::fromLocalPath(mp3));
    REQUIRE(sibling.size() == taglib.size());
    REQUIRE(sibling.size() == 3);
    for (std::size_t i = 0; i < sibling.size(); ++i) {
        CHECK(sibling[i].startMs == taglib[i].startMs);
        CHECK(sibling[i].title == taglib[i].title);
        CHECK(sibling[i].id == taglib[i].id);
    }
}

#if !XPCOG_WITH_FFMPEG

TEST_CASE("a chapter row opens on its span with its title", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("span");
    REQUIRE_FALSE(mp3.empty());
    MetadataMap tags;
    tags.set("title", "The Book");
    REQUIRE(writeTagsAndChapters(mp3, tags, threeChapters()));

    const Url book = Url::fromLocalPath(mp3);
    const auto opened = registry().open(book.withFragment("1"));
    REQUIRE(opened);
    // 300 ms at 44.1 kHz, rounded to nearest as av_rescale_q does.
    CHECK(opened.decoder->properties().totalFrames == 13230);
    const MetadataMap rowTags = opened.decoder->metadata();
    CHECK(rowTags.first("title") == "Two");
    CHECK(rowTags.first("album") == "The Book");

    // The span is exact: reading to the end yields precisely its frames.
    std::int64_t read = 0;
    AudioChunk   chunk;
    while (opened.decoder->readAudio(chunk)) {
        read += static_cast<std::int64_t>(chunk.frameCount());
    }
    CHECK(read == 13230);
    CHECK(opened.decoder->seek(0) == 0);
}

TEST_CASE("chapter 0 starts at the first real sample", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("gapless-zero");
    REQUIRE_FALSE(mp3.empty());
    REQUIRE(writeTagsAndChapters(mp3, {}, threeChapters()));

    // The reference is the whole file from sample zero -- which the ex layer
    // measures from the first real audio sample, LAME's delay already
    // removed. Chapter 0 must land on exactly those samples.
    const auto whole = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(whole);
    const std::vector<float> reference = decodeAll(*whole.decoder);
    REQUIRE(reference.size() > 2304);

    const auto opened = registry().open(Url::fromLocalPath(mp3).withFragment("0"));
    REQUIRE(opened);
    const std::vector<float> start = readFrames(*opened.decoder, 2304);
    REQUIRE(start.size() == 2304);
    CHECK(worstDifference(start, reference, 0, 2304) < 1e-3f);
}

TEST_CASE("seeking within a chapter lands inside it", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("chapter-seek");
    REQUIRE_FALSE(mp3.empty());
    REQUIRE(writeTagsAndChapters(mp3, {}, threeChapters()));

    const auto whole = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(whole);
    const std::vector<float> reference = decodeAll(*whole.decoder);

    const auto opened = registry().open(Url::fromLocalPath(mp3).withFragment("1"));
    REQUIRE(opened);
    const std::int64_t span = opened.decoder->properties().totalFrames;
    const std::int64_t mid  = span / 2;
    REQUIRE(opened.decoder->seek(mid) == mid);
    const std::vector<float> window = readFrames(*opened.decoder, 2304);
    REQUIRE(window.size() == 2304);
    // Chapter 1 starts at frame 13230; the seek is relative to it.
    CHECK(worstDifference(window, reference, 13230 + static_cast<std::size_t>(mid), 2304) <
          1e-3f);
}

TEST_CASE("non-numeric fragments and streams refuse", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("refusals");
    REQUIRE_FALSE(mp3.empty());
    REQUIRE(writeTagsAndChapters(mp3, {}, threeChapters()));

    CHECK_FALSE(registry().open(Url::fromLocalPath(mp3).withFragment("abc")));
    CHECK_FALSE(registry().open(Url::fromLocalPath(mp3).withFragment("-1")));
    CHECK_FALSE(registry().open(Url::fromLocalPath(mp3).withFragment("99")));
    CHECK_FALSE(streamingRegistry().open(streamUrl(mp3).withFragment("1")));
}

TEST_CASE("removing chapters returns the file to whole-file playback", "[mp3][chapters]") {
    if (!xpcog::test::haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path mp3 = makeMp3("removal");
    REQUIRE_FALSE(mp3.empty());
    REQUIRE(writeTagsAndChapters(mp3, {}, threeChapters()));
    REQUIRE(registry().open(Url::fromLocalPath(mp3).withFragment("1")));

    // An empty list removes them; the fragment then names nothing.
    MetadataWriteOptions options;
    REQUIRE(registry().writeMetadata(Url::fromLocalPath(mp3), {}, ChapterList{}, options));
    CHECK_FALSE(registry().open(Url::fromLocalPath(mp3).withFragment("0")));
    const auto whole = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(whole);
    CHECK(std::abs(whole.decoder->properties().totalFrames - kRate) <= 1);
}

#endif  // !XPCOG_WITH_FFMPEG
