// 2SF: that melonDS renders a Nintendo DS's audio and that the track ends where
// the tags say.
//
// The 2SF container is the awkward one of the family: `exe` carries the ROM as
// offset/length chunks and `reserved` carries zlib-compressed SAVE chunks in
// the same format, each with its own CRC. A core that assembles the cartridge
// at the wrong offsets boots a DS full of zeros -- the chain resolves, the tags
// are right, and nothing plays. Only looking at samples catches that.
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

/// Up to `want` playable GSFs, taken from as many different games as possible.
/// One game's rips share a `.gsflib` and so share a sample rate; six files from
/// one set would test the rate once.
[[nodiscard]] std::vector<fs::path> findTwoSfs(std::size_t want) {
    return findPsfFiles({".2sf", ".mini2sf"}, {.want = want, .onePerDirectory = true});
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

/// Five seconds at the DS rate: long enough to get past a game booting.
constexpr std::size_t kFiveSeconds = 163642;

}  // namespace

TEST_CASE("the 2SF decoder is registered for both spellings", "[2sf]") {
    CHECK(registry().isPlayableExtension("2sf"));
    CHECK(registry().isPlayableExtension("mini2sf"));
    CHECK_FALSE(registry().isPlayableExtension("2sflib"));
}

TEST_CASE("a 2SF renders audio, not silence", "[2sf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto twosfs = findTwoSfs(3);
    if (twosfs.empty()) {
        SKIP("corpus holds no 2SF files");
    }

    for (const fs::path& path : twosfs) {
        INFO(path.filename().string());

        // Long enough to get past the boot. A 2SF starts with the game
        // starting: unless the rip states `_2sf_initial_frames`, and none in
        // this corpus does, the first second or so is a DS powering on. Cog has
        // the same behaviour, so this looks for sound within five seconds
        // rather than immediately.
        const Decoded decoded = decode(path, kFiveSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);

        // The cartridge was assembled at the right offsets and melonDS ran it.
        // Get the map offsets wrong and the DS boots a cartridge of zeros: the
        // `_lib` chain still resolves and the duration is still right.
        INFO("peak over five seconds: " << peak(decoded.samples, 0, kFiveSeconds));
        CHECK(peak(decoded.samples, 0, kFiveSeconds) > 500);

        // And that it was this decoder rather than vgmstream, which also claims
        // `2sf` among its several hundred extensions.
        CHECK(decoded.properties.codec == "2SF");
    }
}

TEST_CASE("the 2SF sample rate is the DS SPU's", "[2sf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto twosfs = findTwoSfs(2);
    if (twosfs.empty()) {
        SKIP("corpus holds no 2SF files");
    }

    // The DS SPU runs at the ARM7 clock over 1024, which is 32728.498 Hz and
    // not any round number near it. Rounding to 32728 or 32768 is a slow drift
    // rather than an obvious fault, so it is pinned exactly. Unlike the GBA
    // there is nothing dynamic about it -- the rate is the hardware's, not the
    // game's, which is why this core can open lazily and the GSF one cannot.
    constexpr double kExpected = 33513982.0 / 1024.0;
    for (const fs::path& path : twosfs) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(static_cast<bool>(opened));
        CHECK_THAT(opened.decoder->properties().format.sampleRate,
                   Catch::Matchers::WithinAbs(kExpected, 1e-6));
    }
}

TEST_CASE("opening a 2SF does not boot a DS", "[2sf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto twosfs =
        findPsfFiles({".2sf", ".mini2sf"}, {.want = 1, .chainedOnly = true});
    if (twosfs.empty()) {
        SKIP("corpus holds no chained 2SF");
    }

    // Same contract as the USF core, and testable the same way: the tags-only
    // path stops before psflib follows `_lib`, so a mini-PSF carried away from
    // its library opens, reports a duration, and then has no cartridge to run.
    // Eager loading could not do that -- it resolves the chain up front.
    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-2sf-orphan" / twosfs.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(twosfs.front(), orphan, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(orphan));
    REQUIRE(static_cast<bool>(opened));
    CHECK(opened.decoder->properties().totalFrames > 0);

    AudioChunk chunk;
    CHECK_FALSE(opened.decoder->readAudio(chunk));

    opened.decoder->close();
    opened.decoder.reset();
    fs::remove_all(orphan.parent_path(), error);
}

TEST_CASE("a 2SF honours the length and fade tags", "[2sf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto twosfs = findTwoSfs(6);
    if (twosfs.empty()) {
        SKIP("corpus holds no 2SF files");
    }

    int checked = 0;
    for (const fs::path& path : twosfs) {
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

TEST_CASE("the 2SF core refuses another console's PSF", "[2sf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto others = findPsfFiles({".minigsf"}, {.want = 1});
    if (others.empty()) {
        SKIP("corpus holds no GSF to check the version byte against");
    }

    INFO(others.front().filename().string());
    const Url url = Url::fromLocalPath(others.front());
    CHECK(loadPsf(url, registry()).has_value());
    CHECK_FALSE(loadPsf(url, registry(), 0x24).has_value());
}
