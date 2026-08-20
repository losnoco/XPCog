// NCSF: that SSEQPlayer performs a DS sequence, and that the sequence number
// actually selects one.
//
// This core is not an emulator. An NCSF's `exe` is an SDAT -- the DS's sound
// archive -- and its `reserved` is four bytes naming which SSEQ inside that
// archive to play. Every track in a set shares one `.ncsflib`, so the sequence
// number is the *only* thing distinguishing them: ignore it and all 45 files
// decode successfully, report their own titles and lengths, and play the same
// music. That is the failure this file exists to catch, and it is the analogue
// of the wrong-overlay-order failure in the emulator cores.
//
// Rips cannot be committed, so these run against a corpus already on the
// machine (`-DXPCOG_PSF_CORPUS=<path>`) and skip without one.

#include "PsfCorpus.hpp"
#include "psf/PsfFile.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::codecs;
using namespace xpcog::testing;
namespace fs = std::filesystem;

namespace {

PluginRegistry& registry() { return psfRegistry(); }

/// Up to `want` playable NCSFs, one per directory -- one game's rips share a
/// `.snsflib` and testing six of them tests one cartridge.
[[nodiscard]] std::vector<fs::path> findNcsfs(std::size_t want,
                                              bool onePerDirectory = true) {
    return findPsfFiles({".ncsf", ".minincsf"},
                        {.want = want, .onePerDirectory = onePerDirectory});
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
    std::size_t frames() const { return samples.size() / 2; }
};

[[nodiscard]] Decoded decode(const fs::path& path, std::size_t limit,
                             std::int64_t seekTo = -1) {
    Decoded out;
    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();
    if (seekTo >= 0 && opened.decoder->seek(seekTo) != seekTo) {
        out.properties = {};
        return out;
    }

    AudioChunk chunk;
    while (out.frames() < limit && opened.decoder->readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t at = out.samples.size();
        out.samples.resize(at + frames * 2);
        std::memcpy(out.samples.data() + at, chunk.bytes().data(),
                    frames * 2 * sizeof(std::int16_t));
    }
    return out;
}

[[nodiscard]] int peak(const std::vector<std::int16_t>& samples, std::size_t from,
                       std::size_t to) {
    int highest = 0;
    for (std::size_t i = from * 2; i < std::min(to * 2, samples.size()); ++i) {
        highest = std::max(highest, std::abs(static_cast<int>(samples[i])));
    }
    return highest;
}

/// Ten seconds at the rate SSEQPlayer is asked to synthesise.
constexpr std::size_t kTenSeconds = 441000;

}  // namespace

TEST_CASE("the NCSF decoder is registered for both spellings", "[ncsf]") {
    CHECK(registry().isPlayableExtension("ncsf"));
    CHECK(registry().isPlayableExtension("minincsf"));
    CHECK_FALSE(registry().isPlayableExtension("ncsflib"));
}

TEST_CASE("an NCSF renders audio, not silence", "[ncsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto ncsfs = findNcsfs(3);
    if (ncsfs.empty()) {
        SKIP("corpus holds no NCSF files");
    }

    for (const fs::path& path : ncsfs) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTenSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.codec == "NCSF");

        INFO("peak over ten seconds: " << peak(decoded.samples, 0, kTenSeconds));
        CHECK(peak(decoded.samples, 0, kTenSeconds) > 500);
    }
}

TEST_CASE("the sequence number selects the track", "[ncsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto ncsfs = findNcsfs(4, /*onePerDirectory=*/false);
    if (ncsfs.size() < 2) {
        SKIP("corpus holds fewer than two NCSF files");
    }

    // Two files from the same set share an SDAT and differ only in the four
    // bytes of `reserved`. If those were dropped -- or if the chain handed the
    // library's copy over in place of the file's -- both would play sequence
    // zero, and everything else about them would still be right.
    constexpr std::size_t kCompare = 44100 * 8;
    const Decoded first  = decode(ncsfs[0], kCompare);
    const Decoded second = decode(ncsfs[1], kCompare);
    REQUIRE(first.frames() > 0);
    REQUIRE(second.frames() > 0);
    INFO(ncsfs[0].filename().string() << " vs " << ncsfs[1].filename().string());

    const std::size_t common = std::min(first.frames(), second.frames()) * 2;
    REQUIRE(common > 0);
    const bool identical = std::equal(first.samples.begin(),
                                      first.samples.begin() +
                                          static_cast<std::ptrdiff_t>(common),
                                      second.samples.begin());
    CHECK_FALSE(identical);
}

TEST_CASE("an NCSF honours the length and fade tags", "[ncsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto ncsfs = findNcsfs(6, /*onePerDirectory=*/false);
    if (ncsfs.empty()) {
        SKIP("corpus holds no NCSF files");
    }

    int checked = 0;
    for (const fs::path& path : ncsfs) {
        const auto tags = readPsfTags(Url::fromLocalPath(path), registry());
        if (!tags || !tags->length || *tags->length <= 0.0) {
            continue;
        }
        INFO(path.filename().string());

        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(static_cast<bool>(opened));

        const TrackProperties props = opened.decoder->properties();
        const double expected =
            (*tags->length + tags->fade.value_or(0.0)) * props.format.sampleRate;
        CHECK_THAT(static_cast<double>(props.totalFrames),
                   Catch::Matchers::WithinAbs(expected, 2.0));
        ++checked;
    }
    INFO("files carrying a length tag: " << checked);
    CHECK(checked > 0);
}

TEST_CASE("opening an NCSF does not parse the archive", "[ncsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto ncsfs =
        findPsfFiles({".ncsf", ".minincsf"}, {.want = 1, .chainedOnly = true});
    if (ncsfs.empty()) {
        SKIP("corpus holds no chained NCSF");
    }

    // One .ncsflib serves a whole set, so the cheap path matters here as much
    // as anywhere: listing 45 tracks should not parse the archive 45 times.
    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-ncsf-orphan" / ncsfs.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(ncsfs.front(), orphan, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(orphan));
    REQUIRE(static_cast<bool>(opened));
    CHECK(opened.decoder->properties().totalFrames > 0);

    // And with the library gone there is no archive to perform. SSEQPlayer
    // reports that by throwing; a decoder that declines rather than a process
    // that dies is the whole point of catching it.
    AudioChunk chunk;
    CHECK_FALSE(opened.decoder->readAudio(chunk));

    opened.decoder->close();
    opened.decoder.reset();
    fs::remove_all(orphan.parent_path(), error);
}

TEST_CASE("the NCSF core refuses another console's PSF", "[ncsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto others = findPsfFiles(
        {".minigsf", ".miniusf", ".mini2sf", ".minisnsf", ".minissf", ".dsf"},
        {.want = 1});
    if (others.empty()) {
        SKIP("corpus holds no other PSF to check the version byte against");
    }

    INFO(others.front().filename().string());
    const Url url = Url::fromLocalPath(others.front());
    CHECK(loadPsf(url, registry()).has_value());
    CHECK_FALSE(loadPsf(url, registry(), 0x25).has_value());
}
