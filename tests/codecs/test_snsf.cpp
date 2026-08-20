// SNSF: that snes9x renders a Super Nintendo's audio, for the whole length of a
// track, without the stream drying up.
//
// The length matters more here than in the other cores. An SNSF is a cartridge
// because the SPC700's 64 KB of audio RAM cannot hold everything a game plays:
// the famous case streams sample chunks from ROM through the CPU-APU I/O ports
// while the music runs. If that handshake slips, the audio does not stop -- it
// thins out, or a buffer repeats, somewhere in the middle of a track that
// started fine. So these decode minutes rather than seconds and look at how the
// content varies across them.
//
// SNSF's sections are laid out differently again: the *first* section's offset
// becomes a base that later sections are biased by, then masked into the
// cartridge window. Get that wrong and the cartridge is assembled at the wrong
// addresses, which boots to silence.
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

/// Up to `want` playable SNSFs, one per directory -- one game's rips share a
/// `.snsflib` and testing six of them tests one cartridge.
[[nodiscard]] std::vector<fs::path> findSnsfs(std::size_t want,
                                              bool onePerDirectory = true) {
    return findPsfFiles({".snsf", ".minisnsf"},
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

/// Ten seconds at the S-DSP's 32 kHz.
constexpr std::size_t kTenSeconds = 320000;

}  // namespace

TEST_CASE("the SNSF decoder is registered for both spellings", "[snsf]") {
    CHECK(registry().isPlayableExtension("snsf"));
    CHECK(registry().isPlayableExtension("minisnsf"));
    CHECK_FALSE(registry().isPlayableExtension("snsflib"));
}

TEST_CASE("an SNSF renders audio, not silence", "[snsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto snsfs = findSnsfs(3);
    if (snsfs.empty()) {
        SKIP("corpus holds no SNSF files");
    }

    for (const fs::path& path : snsfs) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTenSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.sampleRate == 32000.0);

        // The cartridge was assembled at the right addresses. Get the section
        // base wrong and the console boots to silence with the chain, the tags
        // and the duration all still correct.
        INFO("frames decoded: " << decoded.frames() << " of " << kTenSeconds);
        INFO("peak over ten seconds: " << peak(decoded.samples, 0, kTenSeconds));
        CHECK(peak(decoded.samples, 0, kTenSeconds) > 500);

        CHECK(decoded.properties.codec == "SNSF");
    }
}

TEST_CASE("an SNSF does not stop producing partway through", "[snsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto snsfs = findSnsfs(1);
    if (snsfs.empty()) {
        SKIP("corpus holds no SNSF files");
    }
    INFO(snsfs.front().filename().string());

    // The failure this exists for does not look like a failure at the start.
    // A driver that streams its samples through the CPU keeps playing only for
    // as long as the handshake holds; when it slips, the emulator stops handing
    // frames over partway into a track that began perfectly.
    //
    // Deliberately about frames rather than loudness. A rip set contains sound
    // effects as well as music -- `sfx-005F` in the set this was written
    // against plays for two seconds and is silent for the remaining 156 -- so
    // "there is audio at 45 seconds" is a property of music, not of SNSF. What
    // is true of every entry is that the decoder keeps producing frames until
    // the declared length runs out.
    constexpr std::size_t kMinute = 32000 * 60;
    const Decoded decoded = decode(snsfs.front(), kMinute);
    REQUIRE(decoded.properties.totalFrames > static_cast<std::int64_t>(kMinute));
    INFO("frames decoded: " << decoded.frames() << " of " << kMinute);
    CHECK(decoded.frames() == kMinute);
}

TEST_CASE("a long SNSF track keeps changing", "[snsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // A starved stream can also replay what it already has rather than stop, so
    // this looks for a track with real duration and checks its content varies.
    // Candidates come from one directory as well as several, because a set is
    // often a single game, and the first entry alphabetically may be a sound
    // effect rather than a song.
    constexpr std::size_t kSecond = 32000;
    for (const fs::path& path : findSnsfs(8, /*onePerDirectory=*/false)) {
        const Decoded decoded = decode(path, kSecond * 45);
        if (decoded.frames() < kSecond * 45) {
            continue;
        }

        std::vector<std::size_t> loud;
        for (std::size_t second = 0; second < 45; ++second) {
            if (peak(decoded.samples, second * kSecond, (second + 1) * kSecond) > 500) {
                loud.push_back(second);
            }
        }
        if (loud.size() < 15) {
            continue;  // a sound effect, or a short cue: not what this tests
        }
        INFO(path.filename().string() << ", " << loud.size() << " loud seconds");

        // No two of them identical. A buffer being replayed is what a starved
        // stream sounds like when it does not simply stop.
        int repeats = 0;
        for (std::size_t i = 1; i < loud.size(); ++i) {
            const auto* a = decoded.samples.data() + loud[i - 1] * kSecond * 2;
            const auto* b = decoded.samples.data() + loud[i] * kSecond * 2;
            if (std::memcmp(a, b, kSecond * 2 * sizeof(std::int16_t)) == 0) {
                ++repeats;
            }
        }
        INFO("identical consecutive loud seconds: " << repeats);
        CHECK(repeats == 0);
        return;
    }
    SKIP("corpus holds no SNSF track long enough to test for a stalled stream");
}

TEST_CASE("opening an SNSF does not boot a console", "[snsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto snsfs =
        findPsfFiles({".snsf", ".minisnsf"}, {.want = 1, .chainedOnly = true});
    if (snsfs.empty()) {
        SKIP("corpus holds no chained SNSF");
    }

    // A .minisnsf can be fifty bytes against a six-megabyte .snsflib, so the
    // cheap path matters here more than anywhere: a 373-track set should not
    // inflate the cartridge 373 times to be listed.
    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-snsf-orphan" / snsfs.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(snsfs.front(), orphan, fs::copy_options::overwrite_existing, error);
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

TEST_CASE("the SNSF core refuses another console's PSF", "[snsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto others = findPsfFiles({".minigsf", ".miniusf", ".mini2sf"}, {.want = 1});
    if (others.empty()) {
        SKIP("corpus holds no other PSF to check the version byte against");
    }

    INFO(others.front().filename().string());
    const Url url = Url::fromLocalPath(others.front());
    CHECK(loadPsf(url, registry()).has_value());
    CHECK_FALSE(loadPsf(url, registry(), 0x23).has_value());
}
