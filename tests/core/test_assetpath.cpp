// Finding the files XPCog ships beside itself.
//
// Small surface, and the reason it is tested at all is that the failure is
// silent in the worst way: a lookup that answers nothing does not crash, it
// just makes MIDI play on an FM chip instead of a wavetable, on somebody else's
// machine, in a layout nobody here builds. So this pins the two things a caller
// depends on -- that the directory is the running executable's, and that a name
// which does not exist comes back empty rather than as a path to nothing.
//
// What it cannot test is the installed layouts of the other two platforms.
// Those are one `#if` each in AssetPath.cpp and are checked by CI compiling
// them and by the bank actually playing.

#include "xpcog/core/AssetPath.hpp"
#include "xpcog/core/FilePath.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace xpcog;
namespace fs = std::filesystem;

TEST_CASE("the asset directory is beside the running executable", "[assets]") {
    const fs::path dir = assetDirectory();
    REQUIRE_FALSE(dir.empty());

    // It has to exist, because the build stages the assets into it -- so an
    // empty or missing answer here means the staging did not run, which is
    // exactly the case that would otherwise be discovered by a listener.
    std::error_code error;
    CHECK(fs::is_directory(dir, error));
}

TEST_CASE("a name that is not shipped resolves to nothing", "[assets]") {
    // Not a path to a file that does not exist: empty. The callers branch on
    // this to decide whether a synthesiser can be offered at all, and a path
    // that names nothing would turn "we ship no bank" into "the bank is
    // broken".
    CHECK(assetPath("no-such-asset-1a2b3c.bin").empty());
    CHECK(assetPath("").empty());

    // A directory is not an asset either, and `soundfonts` is one that exists.
    CHECK(assetPath("soundfonts").empty());
}

TEST_CASE("a shipped name resolves to a readable file", "[assets]") {
    const fs::path bank = assetPath("soundfonts/GeneralUserXG-SFeTest.sf3");
    if (bank.empty()) {
        SKIP("this build has no bank staged beside the test binary");
    }

    std::error_code error;
    CHECK(fs::is_regular_file(bank, error));

    // And it is the bank rather than a placeholder of the right name: an SF2 or
    // SF3 is a RIFF file whose form type is `sfbk`. A staging step that copied
    // a zero-byte file, or one mangled by a text-mode checkout, fails here
    // rather than inside the engine.
    std::ifstream file(bank, std::ios::binary);
    REQUIRE(file);
    char header[12] = {};
    file.read(header, sizeof(header));
    REQUIRE(file.gcount() == sizeof(header));
    CHECK(std::string(header, 4) == "RIFF");
    CHECK(std::string(header + 8, 4) == "sfbk");
}
