// Tag writing through the TagLib codec: write through the registry, read
// back through the registry, and check what survives.
//
// The expectations here are the semantics settled by probing TagLib against
// real fixtures (see the header of codecs/taglib/TagLibWriter.cpp): replace
// vs. merge, empty value lists as deletions, albumart bytes as add/clear,
// per-family capabilities. Fixtures are made with the command-line encoders
// and skip, not fail, when a tool is missing -- docs/PORTING.md: watch the
// skip count.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace xpcog;
using xpcog::test::haveTool;
using xpcog::test::kSilenceStderr;

namespace {

PluginRegistry& registry() {
    static PluginRegistry       instance;
    static const bool           once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// Scratch fixtures, wiped per test case so one write never sees the last
/// test's tags.
fs::path freshDir(const std::string& name) {
    static const fs::path root = [] {
        const fs::path dir = fs::temp_directory_path() / "xpcog-tagwriter-tests";
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

/// One second of 16-bit stereo 44.1 kHz silence; these tests are about tags,
/// not audio. Same fixture as test_scanner.cpp.
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

bool makeTaggedFlac(const fs::path& target, const std::vector<std::string>& tags,
                    const fs::path& picture = {}) {
    if (!haveTool("flac")) {
        return false;
    }
    const fs::path wav = target.parent_path() / "source.wav";
    writeSilentWav(wav);

    std::string command = "flac -s -f --totally-silent";
    for (const std::string& tag : tags) {
        command += " --tag=\"" + tag + "\"";
    }
    if (!picture.empty()) {
        command += " --picture=\"" + picture.string() + "\"";
    }
    command += " -o \"" + target.string() + "\" \"" + wav.string() + "\"";
    command += kSilenceStderr;

    const bool ok = std::system(command.c_str()) == 0;
    std::error_code ignored;
    fs::remove(wav, ignored);
    return ok && fs::exists(target);
}

/// Encode a silent wav with ffmpeg; skips callers when the tool is missing.
/// -map and -metadata belong after -i, and ffmpeg cannot attach a cover to
/// mp3 or m4a output, so every cover in these tests goes in through the
/// writer itself.
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

bool makeWv(const fs::path& wav, const fs::path& target) {
    if (!haveTool("wavpack")) {
        return false;
    }
    // wavpack prompts before overwriting, and only reads wav, not flac.
    std::error_code ignored;
    fs::remove(target, ignored);
    const std::string command = "wavpack -q -f \"" + wav.string() + "\" \"" + target.string()
                                + "\"" + kSilenceStderr;
    return std::system(command.c_str()) == 0 && fs::exists(target);
}

/// The smallest PNG there is; flac --picture reads the header, and the
/// writer sniffs its magic for the mime label.
fs::path makePng(const fs::path& path) {
    static constexpr unsigned char kPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
        0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
        0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44,
        0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
        0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
        0x60, 0x82};
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(kPng), static_cast<std::streamsize>(sizeof kPng));
    return path;
}

std::vector<std::byte> bytesOf(const fs::path& path) {
    std::ifstream          in{path, std::ios::binary};
    std::vector<std::byte> out;
    for (int c = in.get(); c != std::char_traits<char>::eof(); c = in.get()) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

/// Case-insensitive value lookup. The writer stores Vorbis-comment names
/// lowercased, but TagLib hands the container's own spelling back on read,
/// so a multi-value tag written as "genre" can read back as "GENRE".
const std::vector<std::string>* valuesOf(const MetadataMap& tags, std::string_view key) {
    for (const auto& entry : tags) {
        if (lowered(entry.key) == lowered(std::string{key})) {
            if (const auto* values = std::get_if<std::vector<std::string>>(&entry.value)) {
                return values;
            }
        }
    }
    return nullptr;
}

std::vector<std::string> sortedValues(const MetadataMap& tags, std::string_view key) {
    const auto* values = valuesOf(tags, key);
    if (values == nullptr) {
        return {};
    }
    std::vector<std::string> out = *values;
    std::sort(out.begin(), out.end());
    return out;
}

/// Single-value expectation, spelled the way these tests read.
std::vector<std::string> one(std::string value) { return {std::move(value)}; }

MetadataMap readBack(const fs::path& path) {
    return registry().readMetadata(Url::fromLocalPath(path));
}

bool writeTags(const fs::path& path, const MetadataMap& tags, MetadataWriteOptions& options) {
    return registry().writeMetadata(Url::fromLocalPath(path), tags, options);
}

}  // namespace

TEST_CASE("a FLAC rewrite keeps multi-value and custom names", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-roundtrip");
    if (!makeTaggedFlac(dir / "track.flac", {"TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    MetadataMap tags;
    tags.set("artist", std::vector<std::string>{"Pink Floyd", "Bob Ezrin"});
    tags.set("title", "Dogis");
    tags.set("genre", std::vector<std::string>{"rock", "progressive"});
    tags.set("customnote", "arranged in a hotel room");
    tags.set("albumartist", "Björk");  // utf-8, for the round trip

    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.flac", tags, options));

    const MetadataMap back = readBack(dir / "track.flac");
    CHECK(sortedValues(back, "artist") == std::vector<std::string>{"Bob Ezrin", "Pink Floyd"});
    CHECK(sortedValues(back, "genre")
          == std::vector<std::string>{"progressive", "rock"});
    CHECK(sortedValues(back, "title") == one("Dogis"));
    // Vorbis comments take names the spec never heard of, and keep them as
    // written: the capability flags and the bytes agree.
    CHECK(sortedValues(back, "customnote") == one("arranged in a hotel room"));
    CHECK(sortedValues(back, "albumartist") == one("Björk"));
}

TEST_CASE("a replace write drops what the editor left out", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-replace");
    if (!makeTaggedFlac(dir / "track.flac", {"ARTIST=Pink Floyd", "TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    // The edit map carries one value and nothing else; the old artist and
    // the old embedded cover both go.
    const fs::path png = makePng(dir / "cover.png");
    REQUIRE(makeTaggedFlac(dir / "replaced.flac", {"ARTIST=Pink Floyd", "TITLE=Dogs"}, png));

    MetadataMap tags;
    tags.set("title", "Sheep");
    MetadataWriteOptions options;  // stripExisting is the default
    REQUIRE(writeTags(dir / "replaced.flac", tags, options));

    const MetadataMap back = readBack(dir / "replaced.flac");
    CHECK(sortedValues(back, "title") == one("Sheep"));
    CHECK(valuesOf(back, "artist") == nullptr);
    CHECK(back.bytes("albumart") == nullptr);

    // And the plain file's untouched tag survived its own untouched pass:
    // nothing was written to it.
    CHECK(sortedValues(readBack(dir / "track.flac"), "artist") == one("Pink Floyd"));
}

TEST_CASE("a merge write keeps what the editor did not touch", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-merge");
    if (!makeTaggedFlac(dir / "track.flac", {"ARTIST=Pink Floyd", "TITLE=Dogs", "GENRE=rock"})) {
        SKIP("flac is not installed");
    }

    MetadataMap tags;
    tags.set("title", "Sheep");
    MetadataWriteOptions options;
    options.stripExisting = false;
    REQUIRE(writeTags(dir / "track.flac", tags, options));

    MetadataMap back = readBack(dir / "track.flac");
    CHECK(sortedValues(back, "title") == one("Sheep"));
    CHECK(sortedValues(back, "artist") == one("Pink Floyd"));
    CHECK(valuesOf(back, "genre") != nullptr);

    // The merge is idempotent: TagLib uppercases names on read-back, and a
    // naive merge would write "genre" a second time under the other
    // spelling. Two merge passes must leave one value.
    REQUIRE(writeTags(dir / "track.flac", tags, options));
    back = readBack(dir / "track.flac");
    CHECK(sortedValues(back, "genre") == std::vector<std::string>{"rock"});
    CHECK(sortedValues(back, "artist") == std::vector<std::string>{"Pink Floyd"});
}

TEST_CASE("an empty value list deletes the tag", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-delete");
    if (!makeTaggedFlac(dir / "track.flac", {"ARTIST=Pink Floyd", "TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    MetadataMap tags;
    tags.set("artist", std::vector<std::string>{});  // delete
    tags.set("comment", "kept");
    MetadataWriteOptions options;
    options.stripExisting = false;
    REQUIRE(writeTags(dir / "track.flac", tags, options));

    const MetadataMap back = readBack(dir / "track.flac");
    CHECK(valuesOf(back, "artist") == nullptr);
    CHECK(sortedValues(back, "title") == one("Dogs"));
    CHECK(sortedValues(back, "comment") == one("kept"));
}

TEST_CASE("albumart bytes add and clear the cover", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-art");
    const fs::path png = makePng(dir / "cover.png");
    if (!makeTaggedFlac(dir / "track.flac", {"TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    // Add.
    MetadataMap tags;
    tags.set("title", "Dogs");
    tags.setBytes("albumart", bytesOf(png));
    MetadataWriteOptions options;
    options.stripExisting = false;
    REQUIRE(writeTags(dir / "track.flac", tags, options));
    const MetadataMap added = readBack(dir / "track.flac");
    const auto art = added.bytes("albumart");
    REQUIRE(art != nullptr);
    CHECK(*art == bytesOf(png));

    // Clear, through an empty bytes entry in merge mode.
    tags.setBytes("albumart", std::vector<std::byte>{});
    REQUIRE(writeTags(dir / "track.flac", tags, options));
    CHECK(readBack(dir / "track.flac").bytes("albumart") == nullptr);
}

TEST_CASE("a replace write with no art removes an embedded cover", "[tags][tagwriter]") {
    const fs::path dir = freshDir("flac-stripart");
    const fs::path png = makePng(dir / "cover.png");
    if (!makeTaggedFlac(dir / "track.flac", {"TITLE=Dogs"}, png)) {
        SKIP("flac is not installed");
    }
    REQUIRE(readBack(dir / "track.flac").bytes("albumart") != nullptr);

    // The editor showed the art and the user removed it, so the map it hands
    // over simply has no albumart row; setProperties alone would have left
    // the picture block in place.
    MetadataMap tags;
    tags.set("title", "Sheep");
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.flac", tags, options));

    const MetadataMap back = readBack(dir / "track.flac");
    CHECK(back.bytes("albumart") == nullptr);
    CHECK(sortedValues(back, "title") == one("Sheep"));
}

TEST_CASE("covers round-trip through Ogg Opus", "[tags][tagwriter]") {
    const fs::path dir = freshDir("opus-art");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    const fs::path png = makePng(dir / "cover.png");
    if (!makeFromWav(wav, dir / "track.opus")) {
        SKIP("ffmpeg is not installed");
    }

    MetadataMap tags;
    tags.set("title", "Dogs");
    tags.setBytes("albumart", bytesOf(png));
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.opus", tags, options));

    MetadataMap back = readBack(dir / "track.opus");
    const auto* art = back.bytes("albumart");
    REQUIRE(art != nullptr);
    CHECK(*art == bytesOf(png));

    options.stripExisting = false;
    tags.setBytes("albumart", std::vector<std::byte>{});
    REQUIRE(writeTags(dir / "track.opus", tags, options));
    back = readBack(dir / "track.opus");
    CHECK(back.bytes("albumart") == nullptr);
    CHECK(sortedValues(back, "title") == one("Dogs"));
}

TEST_CASE("an MP3 rewrite keeps repeated values and folds custom names",
          "[tags][tagwriter]") {
    const fs::path dir = freshDir("mp3-roundtrip");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "track.mp3", "-codec:a libmp3lame -b:a 32k")) {
        SKIP("ffmpeg is not installed");
    }

    MetadataMap tags;
    tags.set("artist", std::vector<std::string>{"Pink Floyd", "Bob Ezrin"});
    tags.set("title", "Dogs");
    tags.set("customnote", "folds into a TXX frame");
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.mp3", tags, options));

    // The repeated TPE1 frame survives. Do not insist on the custom name:
    // ID3v2 parks it in a TXX text frame whose read-back spelling TagLib
    // picks, which is exactly why arbitraryKeys is false for this family.
    const MetadataMap back = readBack(dir / "track.mp3");
    CHECK(sortedValues(back, "artist") == std::vector<std::string>{"Bob Ezrin", "Pink Floyd"});
    CHECK(sortedValues(back, "title") == one("Dogs"));
}

TEST_CASE("the ID3v2 version request lands in the header", "[tags][tagwriter]") {
    const fs::path dir = freshDir("mp3-version");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "track.mp3", "-codec:a libmp3lame -b:a 32k")) {
        SKIP("ffmpeg is not installed");
    }

    MetadataMap tags;
    tags.set("title", "Dogs");
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.mp3", tags, options));

    const auto bytes = bytesOf(dir / "track.mp3");
    REQUIRE(bytes.size() > 4);
    REQUIRE(std::string{reinterpret_cast<const char*>(bytes.data()), 3} == "ID3");
    // The fourth byte is the major version of the ID3v2 block written.
    CHECK(static_cast<unsigned char>(bytes[3]) == 4);

    options.id3v2Version = 3;
    REQUIRE(writeTags(dir / "track.mp3", tags, options));
    const auto rewritten = bytesOf(dir / "track.mp3");
    REQUIRE(rewritten.size() > 4);
    CHECK(static_cast<unsigned char>(rewritten[3]) == 3);
}

TEST_CASE("a WavPack rewrite keeps arbitrary names and covers", "[tags][tagwriter]") {
    const fs::path dir = freshDir("wv-roundtrip");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeWv(wav, dir / "track.wv")) {
        SKIP("wavpack is not installed");
    }

    const fs::path png = makePng(dir / "cover.png");
    MetadataMap tags;
    tags.set("artist", std::vector<std::string>{"Pink Floyd", "Bob Ezrin"});
    tags.set("customnote", "APEv2 items take any name");
    tags.setBytes("albumart", bytesOf(png));
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.wv", tags, options));

    const MetadataMap back = readBack(dir / "track.wv");
    CHECK(sortedValues(back, "artist") == std::vector<std::string>{"Bob Ezrin", "Pink Floyd"});
    CHECK(sortedValues(back, "customnote") == one("APEv2 items take any name"));
    // APEv2 keeps cover bytes but loses the mime label; XPCog's albumart is
    // opaque bytes either way.
    const auto* art = back.bytes("albumart");
    REQUIRE(art != nullptr);
    CHECK(*art == bytesOf(png));
}

TEST_CASE("plain WAV tags round-trip", "[tags][tagwriter]") {
    const fs::path dir = freshDir("wav-roundtrip");
    const fs::path wav = dir / "track.wav";
    writeSilentWav(wav);

    MetadataMap tags;
    tags.set("artist", "Björk");
    tags.set("title", "Humanlike");
    MetadataWriteOptions options;
    REQUIRE(writeTags(wav, tags, options));

    const MetadataMap back = readBack(wav);
    CHECK(sortedValues(back, "artist") == one("Björk"));
    CHECK(sortedValues(back, "title") == one("Humanlike"));
}

TEST_CASE("an M4A cover lands and clears", "[tags][tagwriter]") {
    const fs::path dir = freshDir("m4a-art");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "track.m4a")) {
        SKIP("ffmpeg is not installed");
    }

    const fs::path png = makePng(dir / "cover.png");
    MetadataMap tags;
    tags.set("title", "Dogs");
    tags.setBytes("albumart", bytesOf(png));
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.m4a", tags, options));

    MetadataMap back = readBack(dir / "track.m4a");
    const auto* art = back.bytes("albumart");
    REQUIRE(art != nullptr);
    CHECK(*art == bytesOf(png));

    tags.setBytes("albumart", std::vector<std::byte>{});
    options.stripExisting = false;
    REQUIRE(writeTags(dir / "track.m4a", tags, options));
    back = readBack(dir / "track.m4a");
    CHECK(back.bytes("albumart") == nullptr);
    CHECK(sortedValues(back, "title") == one("Dogs"));
}

TEST_CASE("a conservative family still takes the tags that fit", "[tags][tagwriter]") {
    const fs::path dir = freshDir("matroska-roundtrip");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeFromWav(wav, dir / "track.mka")) {
        SKIP("ffmpeg is not installed");
    }

    // Matroska reaches the conservative branch: writable, no promises. The
    // writer still has to save and the standard names still have to land.
    MetadataMap tags;
    tags.set("artist", "Pink Floyd");
    tags.set("title", "Dogs");
    MetadataWriteOptions options;
    REQUIRE(writeTags(dir / "track.mka", tags, options));

    const MetadataMap back = readBack(dir / "track.mka");
    CHECK(valuesOf(back, "artist") != nullptr);
    CHECK(valuesOf(back, "title") != nullptr);
}

TEST_CASE("probes report capabilities per family", "[tags][tagwriter]") {
    const fs::path dir = freshDir("caps");
    const fs::path wav = dir / "source.wav";
    writeSilentWav(wav);
    if (!makeTaggedFlac(dir / "track.flac", {"TITLE=Dogs"})) {
        SKIP("flac is not installed");
    }

    MetadataCapabilities caps = registry().metadataCapabilities(Url::fromLocalPath(dir / "track.flac"));
    REQUIRE(caps.writable);
    CHECK(caps.arbitraryKeys);
    CHECK(caps.multiValue);
    CHECK(caps.pictures);
    CHECK_FALSE(caps.lossyRepresentation);

    if (makeFromWav(wav, dir / "track.mp3", "-codec:a libmp3lame -b:a 32k")) {
        caps = registry().metadataCapabilities(Url::fromLocalPath(dir / "track.mp3"));
        REQUIRE(caps.writable);
        CHECK_FALSE(caps.arbitraryKeys);
        CHECK(caps.multiValue);
        CHECK(caps.pictures);
        CHECK(caps.lossyRepresentation);
    }
    if (makeFromWav(wav, dir / "track.m4a")) {
        caps = registry().metadataCapabilities(Url::fromLocalPath(dir / "track.m4a"));
        REQUIRE(caps.writable);
        CHECK_FALSE(caps.arbitraryKeys);
        CHECK(caps.pictures);
        CHECK(caps.lossyRepresentation);
    }
    if (makeWv(wav, dir / "track.wv")) {
        caps = registry().metadataCapabilities(Url::fromLocalPath(dir / "track.wv"));
        REQUIRE(caps.writable);
        CHECK(caps.arbitraryKeys);
        CHECK(caps.multiValue);
        CHECK(caps.pictures);
    }

    // A WAV that carries no tags at all is still writable: the probe reads
    // the container, not the presence of a tag block.
    writeSilentWav(dir / "bare.wav");
    caps = registry().metadataCapabilities(Url::fromLocalPath(dir / "bare.wav"));
    CHECK(caps.writable);

    // Nothing claims a file that is not there.
    CHECK_FALSE(registry()
                    .metadataCapabilities(Url::fromLocalPath(dir / "nope.flac"))
                    .writable);
}

TEST_CASE("sources that are not files are refused before anything opens",
          "[tags][tagwriter]") {
    MetadataWriteOptions options;
    CHECK_FALSE(registry().writeMetadata(*Url::parse("http://example.com/track.flac"), {}, options));
    CHECK_FALSE(options.error.empty());
    CHECK_FALSE(registry().metadataCapabilities(*Url::parse("http://example.com/track.flac")).writable);

    // A cue fragment names part of a file; no writer sees it either way.
    MetadataWriteOptions fragment;
    CHECK_FALSE(registry().writeMetadata(*Url::parse("file:///tmp/opencode/album.cue#3"), {},
                                         fragment));
    CHECK_FALSE(fragment.error.empty());
}
