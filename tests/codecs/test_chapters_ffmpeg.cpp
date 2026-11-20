// The chapters the tag editor writes are the rows the container lists and
// the spans the decoder plays: chapter N in the editor is #N in the
// playlist. Cog asserts the same agreement (iOS/CogPluginsTests/
// TagWriterTests.swift). Compiled only when FFmpeg is (tests/CMakeLists.txt);
// the fixtures still need the ffmpeg binary at runtime and skip without it.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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
