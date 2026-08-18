// Non-ASCII paths, end to end.
//
// The app hands core its paths as std::string, because that is what
// QString::toStdString() produces -- and it produces UTF-8. On Windows,
// std::filesystem::path built from a std::string does NOT read it as UTF-8: it
// reads it in the active code page. So "Björk" arrives as "BjÃ¶rk", names a
// folder nobody has, and the file reads as unopenable.
//
// These tests are written the way the app actually calls: a UTF-8 std::string
// in, a real file on disk to find at the other end.

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/Utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <string>

using namespace xpcog;
namespace fs = std::filesystem;

namespace {

/// A path's UTF-8 bytes, as the app would have them from Qt.
[[nodiscard]] std::string utf8Of(const fs::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

/// A folder whose name is not representable in every code page, holding one
/// file whose name is not either. Built rather than checked in, so it exists on
/// whatever machine runs this.
[[nodiscard]] fs::path awkwardFile() {
    static const fs::path file = [] {
        const fs::path dir = fs::temp_directory_path() /
                             fs::path(std::u8string(u8"xpcog-Björk - Post"));
        fs::create_directories(dir);
        const fs::path path =
            dir / fs::path(std::u8string(u8"01 It's Oh So Quiet — ½ ×.flac"));
        // Through the path, not through path.string(): the narrow form is the
        // very thing under test, and a fixture that cannot be written would look
        // like a fixture that proves something.
        std::ofstream out{path, std::ios::binary};
        out.write("fLaC", 4);
        return path;
    }();
    return file;
}

}  // namespace

TEST_CASE("a UTF-8 path survives the trip through a URL", "[url][unicode]") {
    const fs::path    file = awkwardFile();
    REQUIRE(fs::exists(file));

    // Exactly what MainWindow does with QUrl::toLocalFile().toStdString().
    const Url url = Url::fromLocalPath(pathFromUtf8(utf8Of(file)));

    const auto back = url.localPath();
    REQUIRE(back.has_value());
    CHECK(fs::exists(*back));
    CHECK(fs::equivalent(*back, file));
}

TEST_CASE("a URL's stored form is UTF-8 whatever the platform's code page is",
          "[url][unicode]") {
    // The URL is what goes into the library database and into a playlist file
    // that another machine may read. Percent-encoded bytes of the active code
    // page would be unreadable anywhere else -- and unreadable here too after a
    // regional settings change.
    const Url url = Url::fromLocalPath(pathFromUtf8(utf8Of(awkwardFile())));

    // "ö" is U+00F6, two bytes in UTF-8: C3 B6.
    CHECK(url.toString().find("%C3%B6") != std::string::npos);
    CHECK(url.toString().find("%F6") == std::string::npos);
}

TEST_CASE("a source opens a file whose name the code page cannot spell",
          "[url][unicode]") {
    // The layer the bug actually showed at: the URL round-tripped well enough to
    // look right in a playlist, and then nothing could open it.
    static PluginRegistry registry;
    static const bool     once = [] {
        registerAllCodecs(registry);
        return true;
    }();
    (void)once;

    const Url url = Url::fromLocalPath(pathFromUtf8(utf8Of(awkwardFile())));

    SourcePtr source = registry.makeSource(url);
    REQUIRE(source != nullptr);
    REQUIRE(source->open(url));

    char header[4] = {};
    CHECK(source->read(header, 4) == 4);
    CHECK(std::string(header, 4) == "fLaC");
}

TEST_CASE("path conversions are UTF-8 in both directions", "[url][unicode]") {
    // The pair the rest of this rests on. "ö" is U+00F6: two bytes in UTF-8, and
    // one byte in every code page that has it at all.
    const fs::path path = fs::path(std::u8string(u8"Björk"));
    const std::string utf8 = pathToUtf8(path);

    REQUIRE(utf8.size() == 6);
    CHECK(static_cast<unsigned char>(utf8[2]) == 0xC3);
    CHECK(static_cast<unsigned char>(utf8[3]) == 0xB6);

    CHECK(pathFromUtf8(utf8) == path);
}

TEST_CASE("a URL written by an older build is still readable", "[url][unicode]") {
    // Builds before this stored the platform's narrow form: a library scanned on
    // Windows holds "Hasta Ma%F1ana" where this one writes "Hasta Ma%C3%B1ana".
    // Those rows have to keep working -- there were four of them in the author's
    // own library, and the alternative is a screenful of files that vanished.
    //
    // %F1 alone is not valid UTF-8, which is what makes the old form
    // distinguishable from the new one with no ambiguity to resolve.
    const auto legacy = Url::parse("file:///music/Hasta%20Ma%F1ana.flac");
    REQUIRE(legacy.has_value());

    const auto path = legacy->localPath();
    REQUIRE(path.has_value());

    // Read back the way the build that wrote it would have: those bytes in the
    // platform's narrow encoding. On Windows that turns %F1 into U+00F1 via the
    // code page; on POSIX the byte is the name. Either way it is the path that
    // build was naming, which is the whole point.
    //
    // The byte is written numerically because a hex escape in a string literal
    // is greedy: it would swallow the "a" that follows and mean something else.
    const std::string narrow =
        std::string("/music/Hasta Ma") + static_cast<char>(0xF1) + "ana.flac";
    CHECK(*path == std::filesystem::path{narrow});
}

TEST_CASE("valid UTF-8 is never mistaken for the older form", "[url][unicode]") {
    // The fallback above must not fire on anything this build wrote, or a
    // perfectly good path would be reinterpreted into a wrong one.
    //
    // Bytes are assembled numerically throughout. A hex escape in a string
    // literal swallows the following letter if it happens to be a hex digit, and
    // these test strings are all letters and accents -- so the escape would be
    // testing something other than what it reads as.
    const auto bytes = [](std::initializer_list<int> values) {
        std::string out;
        for (const int value : values) {
            out.push_back(static_cast<char>(value));
        }
        return out;
    };

    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("plain ascii"));
    // "Björk" as this build writes it: ö is U+00F6, the two bytes C3 B6.
    CHECK(isValidUtf8(bytes({'B', 'j', 0xC3, 0xB6, 'r', 'k'})));

    // The same name as an older build stored it -- one code-page byte, which no
    // valid UTF-8 sequence can begin with a continuation of.
    CHECK(!isValidUtf8(bytes({'B', 'j', 0xF6, 'r', 'k'})));
    CHECK(!isValidUtf8(bytes({0xC3})));        // truncated sequence
    CHECK(!isValidUtf8(bytes({0xB6})));        // stray continuation byte
    CHECK(!isValidUtf8(bytes({0xC3, 'a'})));   // lead byte, then not a continuation
}
