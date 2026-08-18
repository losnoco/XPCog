// A Game_Music_Emu sidecar wears the .m3u extension without being a playlist.
//
// Rips of multi-track formats ship one beside the file, carrying per-track names
// and lengths that the rip itself does not hold. The GME decoder hands the whole
// thing to gme_load_m3u_data(); the playlist container must not also read it as
// a track list, because every line names the same file with a suffix no
// filesystem has heard of -- so a folder of rips would fill the playlist with
// entries that cannot open.

#include "xpcog/core/PluginRegistry.hpp"

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

std::filesystem::path writeFile(const std::string& name, const std::string& contents) {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-gme-sidecar-tests";
        std::filesystem::create_directories(path);
        return path;
    }();

    const auto path = dir / name;
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fwrite(contents.data(), 1, contents.size(), file);
    std::fclose(file);
    return path;
}

const bool kHavePlaylists = [] {
    return registry().isContainer(Url::fromLocalPath("dummy.m3u"));
}();

}  // namespace

TEST_CASE("a Game_Music_Emu sidecar is not read as a playlist", "[containers][gme]") {
    if (!kHavePlaylists) {
        SKIP("playlist containers not built");
    }

    std::string sidecar;
    sidecar += "# Donkey Kong\n";
    sidecar += "# Ripped by Izumi.\n";
    sidecar += "\n";
    sidecar += "Donkey Kong.nsf::NSF,1,Track 1,0:00:14.468,,0:00:00\n";
    sidecar += "Donkey Kong.nsf::NSF,2,Track 2,0:00:06.17,,0:00:00\n";

    const auto path = writeFile("rip.m3u", sidecar);
    CHECK(registry().expandContainer(Url::fromLocalPath(path)).empty());
}

TEST_CASE("an IPv6 host is not mistaken for a sidecar", "[containers][gme]") {
    if (!kHavePlaylists) {
        SKIP("playlist containers not built");
    }

    // `::` marks a sidecar entry and is also how an IPv6 host is written.
    // Matching on `::` alone would throw away an ordinary stream entry -- and
    // the comma below is what makes this a genuine trap rather than a
    // theoretical one, since the type token is found by looking for one.
    const auto path =
        writeFile("ipv6.m3u", std::string{"http://[::1]:8000/stream,live.mp3\n"});

    const auto entries = registry().expandContainer(Url::fromLocalPath(path));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].scheme() == "http");
}
