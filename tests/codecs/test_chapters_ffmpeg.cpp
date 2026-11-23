// The chapters the tag editor writes are the rows the container lists and
// the spans the decoder plays: chapter N in the editor is #N in the
// playlist. Cog asserts the same agreement (iOS/CogPluginsTests/
// TagWriterTests.swift). Compiled only when FFmpeg is (tests/CMakeLists.txt);
// the fixtures still need the ffmpeg binary at runtime and skip without it.

#include "common/Id3v2.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using namespace xpcog;
using xpcog::test::haveTool;
using xpcog::test::kSilenceStderr;

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

fs::path freshDir(const std::string& name) {
    static const fs::path root = [] {
        const fs::path dir = fs::temp_directory_path() / "xpcog-chapter-ffmpeg-tests";
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        fs::create_directories(dir);
        return dir;
    }();
    const fs::path dir = root / name;
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    return dir;
}

/// One second of 16-bit stereo 44.1 kHz silence, as in test_taglib_writer.
void writeSilentWav(const fs::path& wav) {
    constexpr int kFrames = 44100;
    std::ofstream out{wav, std::ios::binary};
    const auto    write32 = [&out](std::uint32_t value) {
        const char bytes[4] = {static_cast<char>(value & 0xFF),
                               static_cast<char>((value >> 8) & 0xFF),
                               static_cast<char>((value >> 16) & 0xFF),
                               static_cast<char>((value >> 24) & 0xFF)};
        out.write(bytes, 4);
    };
    const auto write16 = [&out](std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xFF),
                               static_cast<char>((value >> 8) & 0xFF)};
        out.write(bytes, 2);
    };
    const std::uint32_t dataBytes = kFrames * 4;
    out.write("RIFF", 4);
    write32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    write32(16);
    write16(1);
    write16(2);
    write32(44100);
    write32(44100 * 4);
    write16(4);
    write16(16);
    out.write("data", 4);
    write32(dataBytes);
    for (int i = 0; i < kFrames; ++i) {
        write32(0);
    }
}

bool makeFromWav(const fs::path& wav, const fs::path& target, const std::string& args = {}) {
    if (!haveTool("ffmpeg")) {
        return false;
    }
    std::error_code ignored;
    fs::remove(target, ignored);
    const std::string command = "ffmpeg -loglevel error -y -i \"" + wav.string() + "\" " + args
                                + " \"" + target.string() + "\"" + kSilenceStderr;
    return std::system(command.c_str()) == 0 && fs::exists(target);
}

Chapter chapterAt(std::uint64_t startMs, std::string title) {
    Chapter chapter;
    chapter.startMs = startMs;
    chapter.title   = std::move(title);
    return chapter;
}

ChapterList threeChapters() {
    return ChapterList{chapterAt(0, "First"), chapterAt(300, "Second"),
                       chapterAt(600, "Third")};
}

bool writeChapters(const fs::path& path, const ChapterList& chapters) {
    MetadataWriteOptions options;
    options.stripExisting = false;
    return registry().writeMetadata(Url::fromLocalPath(path), {}, chapters, options);
}

/// The container must list exactly the chapters written, as #0..#N-1 in
/// start order, and return the file to one row when they are removed.
void checkExpansionAgreement(const fs::path& file) {
    const Url book = Url::fromLocalPath(file);
    REQUIRE(writeChapters(file, threeChapters()));

    const std::vector<Url> rows = registry().expandContainer(book);
    REQUIRE(rows.size() == 3);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].toString() == book.withFragment(std::to_string(i)).toString());
    }

    REQUIRE(writeChapters(file, ChapterList{}));
    const std::vector<Url> whole = registry().expandContainer(book);
    REQUIRE(whole.size() == 1);
    CHECK(whole.front().toString() == book.toString());
}

/// Chapter 1 runs 300->600 ms and carries its own title; the decoder opens
/// on that span, not the whole file. Encoder granularity rounds the span.
void checkChapterPlayback(const fs::path& file) {
    const Url book = Url::fromLocalPath(file);
    REQUIRE(writeChapters(file, threeChapters()));

    const auto opened = registry().open(book.withFragment("1"));
    REQUIRE(opened);
    const double seconds = opened.decoder->properties().duration();
    CHECK(seconds >= 0.25);
    CHECK(seconds <= 0.35);

    const MetadataMap tags = opened.decoder->metadata();
    const MetaValue*  title = tags.find("title");
    REQUIRE(title != nullptr);
    const auto* values = std::get_if<std::vector<std::string>>(title);
    REQUIRE(values != nullptr);
    CHECK(*values == std::vector<std::string>{"Second"});
}

std::vector<std::uint8_t> fileBytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

/// The same three chapters, muxed by ffmpeg's ffmetadata writer rather than
/// TagLib's -- the other implementation that writes CHAP/CTOC, so the agreement
/// below is checked against both writers, not just the one this project ships.
bool makeFfmpegChapterMp3(const fs::path& wav, const fs::path& mp3) {
    const fs::path plain = mp3.parent_path() / "plain.mp3";
    if (!makeFromWav(wav, plain, "-codec:a libmp3lame -b:a 32k")) {
        return false;
    }
    const fs::path meta = mp3.parent_path() / "chapters.ffmeta";
    {
        std::ofstream out{meta};
        out << ";FFMETADATA1\n";
        for (const auto& [start, end, title] :
             std::vector<std::tuple<int, int, std::string>>{
                 {0, 300, "First"}, {300, 600, "Second"}, {600, 1000, "Third"}}) {
            out << "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" << start << "\nEND=" << end
                << "\ntitle=" << title << "\n";
        }
    }
    std::error_code ignored;
    fs::remove(mp3, ignored);
    const std::string command = "ffmpeg -loglevel error -y -f ffmetadata -i \"" +
                                meta.string() + "\" -i \"" + plain.string() +
                                "\" -c copy -map_metadata 1 -map_chapters 0 \"" +
                                mp3.string() + "\"" + kSilenceStderr;
    return std::system(command.c_str()) == 0 && fs::exists(mp3);
}

/// The narrow parser the minimp3 path reads and the FFmpeg container/decoder
/// the full build plays must agree: same chapters in the same order, starting
/// at the same frames, with interior spans matching to within a frame. The
/// parser's milliseconds become frames at the file's own rate.
void checkParserAgreement(const fs::path& file) {
    const Url book = Url::fromLocalPath(file);

    ChapterList parsed;
    const auto  bytes = fileBytes(file);
    REQUIRE(codecs::parseId3v2Chapters(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(bytes.data()),
                                   bytes.size()},
        parsed));
    REQUIRE(parsed.size() == 3);

    const std::vector<Url> rows = registry().expandContainer(book);
    REQUIRE(rows.size() == parsed.size());

    const auto whole = registry().open(book);
    REQUIRE(whole);
    const double rate = whole.decoder->properties().format.sampleRate;
    REQUIRE(rate > 0.0);
    const auto toFrames = [rate](std::uint64_t ms) {
        return (static_cast<std::int64_t>(ms) * static_cast<std::int64_t>(rate) + 500) / 1000;
    };

    // The chapters tile the file, so the spans before row i sum to its start.
    // That checks every chapter boundary without leaning on the file's end:
    // the last chapter runs to the end of the audio, and the two decoders
    // measure that end differently -- minimp3 trims the encoder's gapless
    // padding, FFmpeg reports the raw length -- so the final tail is not a
    // boundary the two can agree on to the frame. Its start still is.
    std::int64_t played = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].toString() == book.withFragment(std::to_string(i)).toString());
        const auto opened = registry().open(rows[i]);
        REQUIRE(opened);
        // Order: row i carries the parser's chapter i title.
        CHECK(opened.decoder->metadata().first("title") == parsed[i].title);

        const std::int64_t start = toFrames(parsed[i].startMs);
        CHECK(played >= start - 1);
        CHECK(played <= start + 1);

        const std::int64_t got = opened.decoder->properties().totalFrames;
        if (i + 1 < rows.size()) {
            // An interior span ends where the next chapter begins.
            const std::int64_t expect = toFrames(parsed[i + 1].startMs) - start;
            CHECK(got >= expect - 1);
            CHECK(got <= expect + 1);
        }
        played += got;
    }
}

}  // namespace

TEST_CASE("FFmpeg lists an MP3's chapters as the editor wrote them",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("mp3");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "book.mp3", "-codec:a libmp3lame -b:a 32k")) {
        SKIP("libmp3lame is not available");
    }
    checkExpansionAgreement(dir / "book.mp3");
}

TEST_CASE("an MP3 chapter row plays its span, and the whole file still plays",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("mp3-playback");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "book.mp3", "-codec:a libmp3lame -b:a 32k")) {
        SKIP("libmp3lame is not available");
    }
    // minimp3 refuses the fragment, so FFmpeg takes the chapter row; the
    // span and title prove it, since minimp3 would play the whole file.
    checkChapterPlayback(dir / "book.mp3");

    // The whole file still goes to minimp3, which names itself "MP3" where
    // FFmpeg reports the codec's long name.
    const auto whole = registry().open(Url::fromLocalPath(dir / "book.mp3"));
    REQUIRE(whole);
    CHECK(whole.decoder->properties().codec == "MP3");
    const double seconds = whole.decoder->properties().duration();
    CHECK(seconds >= 0.9);
    CHECK(seconds <= 1.1);
}

TEST_CASE("FFmpeg lists an M4A's chapters as the editor wrote them",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("m4a");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "book.m4a")) {
        SKIP("the aac encoder is not available");
    }
    checkExpansionAgreement(dir / "book.m4a");
    checkChapterPlayback(dir / "book.m4a");
}

TEST_CASE("FFmpeg lists a Matroska file's chapters as the editor wrote them",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("mka");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "book.mka")) {
        SKIP("the matroska muxer is not available");
    }
    checkExpansionAgreement(dir / "book.mka");
    checkChapterPlayback(dir / "book.mka");
}

TEST_CASE("the narrow parser and FFmpeg agree on TagLib-written chapters",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("mp3-parser-agreement");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "book.mp3", "-codec:a libmp3lame -b:a 32k")) {
        SKIP("libmp3lame is not available");
    }
    REQUIRE(writeChapters(dir / "book.mp3", threeChapters()));
    checkParserAgreement(dir / "book.mp3");
}

TEST_CASE("the narrow parser and FFmpeg agree on ffmpeg-written chapters",
          "[tags][chapters][ffmpeg]") {
    const fs::path dir = freshDir("mp3-parser-agreement-ffmpeg");
    if (!haveTool("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFfmpegChapterMp3(wav, dir / "book.mp3")) {
        SKIP("libmp3lame is not available");
    }
    checkParserAgreement(dir / "book.mp3");
}
