// M3U and PLS container tests.
//
// These parsers decide what a playlist actually points at, so the interesting
// cases are the awkward ones: relative paths, Windows separators, subsong
// fragments, comments, and PLS entries written out of order.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

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

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-container-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

std::filesystem::path writeFile(const std::string& name, const std::string& contents) {
    const auto path = fixtureDir() / name;
    std::FILE* f    = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    return path;
}

std::vector<Url> expand(const std::filesystem::path& path) {
    return registry().expandContainer(Url::fromLocalPath(path));
}

/// Filename only, so assertions do not depend on the temp directory.
std::string leaf(const Url& url) {
    const auto path = url.localPath();
    return path ? path->filename().string() : std::string{};
}

const bool kHavePlaylists = [] {
    // The containers are optional at configure time.
    return registry().isContainer(Url::fromLocalPath("dummy.m3u"));
}();

}  // namespace

TEST_CASE("M3U skips comments and blank lines", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto path = writeFile("basic.m3u",
                                "#EXTM3U\n"
                                "#EXTINF:123,Artist - Title\n"
                                "first.flac\n"
                                "\n"
                                "   \n"
                                "second.mp3\n"
                                "# trailing comment\n");

    const auto entries = expand(path);
    REQUIRE(entries.size() == 2);
    CHECK(leaf(entries[0]) == "first.flac");
    CHECK(leaf(entries[1]) == "second.mp3");
}

TEST_CASE("M3U resolves relative paths against the playlist", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto path    = writeFile("relative.m3u", "./sub/track.flac\ntrack2.flac\n");
    const auto entries = expand(path);

    REQUIRE(entries.size() == 2);
    // Resolved next to the playlist, not the working directory.
    const auto first = entries[0].localPath();
    REQUIRE(first.has_value());
    CHECK(first->parent_path().filename().string() == "sub");
    // generic_string() on both sides: path::string() is backslash-separated on
    // Windows while the URL round-trip yields forward slashes, so comparing the
    // native forms fails on a difference that is not the one being tested.
    CHECK(first->generic_string().starts_with(fixtureDir().generic_string()));
}

TEST_CASE("M3U converts Windows separators", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto entries = expand(writeFile("windows.m3u", "sub\\dir\\track.flac\n"));
    REQUIRE(entries.size() == 1);
    CHECK(leaf(entries[0]) == "track.flac");

    const auto path = entries[0].localPath();
    REQUIRE(path.has_value());
    CHECK(path->parent_path().filename().string() == "dir");
}

TEST_CASE("M3U preserves a trailing subsong fragment", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    // "#3" is a subsong or cue-track index, not part of the filename.
    const auto entries = expand(writeFile("fragment.m3u", "album.flac#3\n"));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].fragment() == "3");
    CHECK(leaf(entries[0]) == "album.flac");
}

TEST_CASE("M3U keeps a hash that is part of the filename", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    // Only a trailing all-digit run is a fragment; '#' is legal in filenames.
    const auto entries = expand(writeFile("hashname.m3u", "Track #1 Intro.flac\n"));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].fragment().empty());
    CHECK(leaf(entries[0]) == "Track #1 Intro.flac");
}

TEST_CASE("M3U passes absolute URLs through untouched", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto entries =
        expand(writeFile("remote.m3u", "http://example.com/stream.mp3\nlocal.flac\n"));
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].scheme() == "http");
    CHECK(entries[0].toString() == "http://example.com/stream.mp3");
    CHECK(entries[1].scheme() == "file");
}

TEST_CASE("M3U declines HLS manifests", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    // An HLS manifest wears the same extension but is not a track list; it must
    // fall through to the decoder layer rather than being torn into segments.
    // Declining means returning the URL unchanged: returning nothing would make
    // the manifest disappear, since the scanner adds what expansion returns.
    const auto path    = writeFile("hls.m3u8",
                                   "#EXTM3U\n"
                                   "#EXT-X-TARGETDURATION:10\n"
                                   "#EXT-X-MEDIA-SEQUENCE:0\n"
                                   "segment0.ts\n");
    const auto entries = expand(path);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0] == Url::fromLocalPath(path));
}

TEST_CASE("M3U handles CRLF and a UTF-8 BOM", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto entries =
        expand(writeFile("crlf.m3u", "\xEF\xBB\xBF" "one.flac\r\ntwo.flac\r\n"));
    REQUIRE(entries.size() == 2);
    // A retained BOM would corrupt the first filename.
    CHECK(leaf(entries[0]) == "one.flac");
    CHECK(leaf(entries[1]) == "two.flac");
}

TEST_CASE("PLS returns entries in numeric order", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    // Deliberately written out of order: FileN numbering defines the order, not
    // the order the lines happen to appear in.
    const auto entries = expand(writeFile("order.pls",
                                          "[playlist]\n"
                                          "NumberOfEntries=3\n"
                                          "File3=third.flac\n"
                                          "Title3=C\n"
                                          "File1=first.flac\n"
                                          "Title1=A\n"
                                          "File2=second.flac\n"
                                          "Length1=-1\n"));

    REQUIRE(entries.size() == 3);
    CHECK(leaf(entries[0]) == "first.flac");
    CHECK(leaf(entries[1]) == "second.flac");
    CHECK(leaf(entries[2]) == "third.flac");
}

TEST_CASE("PLS ignores non-File keys", "[containers]") {
    if (!kHavePlaylists) SKIP("playlist containers not built");

    const auto entries = expand(writeFile("keys.pls",
                                          "[playlist]\n"
                                          "NumberOfEntries=1\n"
                                          "Version=2\n"
                                          "Title1=Not a file\n"
                                          "Length1=-1\n"
                                          "File1=only.flac\n"
                                          "Filename=bogus.flac\n"));
    REQUIRE(entries.size() == 1);
    CHECK(leaf(entries[0]) == "only.flac");
}

TEST_CASE("a non-container URL expands to itself", "[containers]") {
    // Callers apply expandContainer() uniformly, so a plain track must survive.
    const auto url     = Url::fromLocalPath(fixtureDir() / "plain.flac");
    const auto entries = registry().expandContainer(url);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0] == url);
}
