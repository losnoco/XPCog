// GSF: that mGBA renders a Game Boy Advance's audio, at the rate that GBA
// actually chose, and that the track ends where the tags say.
//
// The rate is the reason this file is not a copy of test_usf.cpp with the
// letters changed. A GBA has no fixed sample rate: the sound hardware runs at
// `0x200 >> SOUNDBIAS.resolution` cycles per sample, and SOUNDBIAS is a register
// the game writes during its own startup. Get it wrong and every track plays at
// the wrong pitch while every other measurement -- duration, peak, fade --
// still passes.
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
[[nodiscard]] std::vector<fs::path> findGsfs(std::size_t want) {
    return findPsfFiles({".gsf", ".minigsf"}, {.want = want, .onePerDirectory = true});
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

constexpr std::size_t kTwoSeconds = 65536;

}  // namespace

TEST_CASE("the GSF decoder is registered for both spellings", "[gsf]") {
    CHECK(registry().isPlayableExtension("gsf"));
    CHECK(registry().isPlayableExtension("minigsf"));
    CHECK_FALSE(registry().isPlayableExtension("gsflib"));
}

TEST_CASE("a GSF renders audio, not silence", "[gsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto gsfs = findGsfs(3);
    if (gsfs.empty()) {
        SKIP("corpus holds no GSF files");
    }

    for (const fs::path& path : gsfs) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTwoSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);

        // That the ROM was assembled and mGBA ran it. A GSF whose `_lib` chain
        // resolved but whose sections were overlaid at the wrong offsets boots
        // a cartridge of zeros and plays nothing.
        INFO("peak: " << peak(decoded.samples, 0, kTwoSeconds));
        CHECK(peak(decoded.samples, 0, kTwoSeconds) > 500);

        // And that it was this decoder, not vgmstream, which also claims `gsf`.
        CHECK(decoded.properties.codec == "GSF");
    }
}

TEST_CASE("the sample rate is the one the GBA chose", "[gsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto gsfs = findGsfs(6);
    if (gsfs.empty()) {
        SKIP("corpus holds no GSF files");
    }

    // A GBA's sound hardware runs at `0x200 >> SOUNDBIAS.resolution` cycles per
    // sample -- 32768 Hz at reset, doubling for each resolution step the game
    // selects during its own startup. The rate is read from the core once it has
    // settled, not declared.
    //
    // The second assertion is the one that matters, and it is here because the
    // bug it guards against shipped. The GBA emits samples from the very first
    // frame, at the reset default, before the game has written SOUNDBIAS -- so a
    // probe that waits only for "some audio exists" stops on frame zero and
    // reports 32768 for every rip in the world. Every other measurement stays
    // correct: duration, peak, fade and the `_lib` chain all pass. What gives it
    // away is the track playing at half speed, which is not something a test
    // hears.
    //
    // What it can see is that 32768 is the value a too-early probe returns, so
    // an entire corpus reporting exactly the reset default is the signature of
    // the regression. That is a statement about a corpus, and it needs enough
    // games in it to be one: a single genuine 32 kHz rip trips it on its own,
    // which is what a corpus holding one `.minigsf` did. So the check is made
    // only where there is a sample to generalise from -- see kEnoughGamesToTell.
    //
    // Golden Sun (AGB-BGOE) is the rip in question and it really does run at
    // 32768. Measured, not assumed: widening the probe to a hundred seconds of
    // emulation with ten seconds of required stability leaves the answer
    // unchanged, so the game never writes SOUNDBIAS at all.
    std::vector<double> rates;
    for (const fs::path& path : gsfs) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(static_cast<bool>(opened));

        const double rate = opened.decoder->properties().format.sampleRate;
        INFO("reported rate: " << rate);

        // 16777216 / (0x200 >> n) for n in 0..4.
        const bool plausible = rate == 32768.0 || rate == 65536.0 || rate == 131072.0 ||
                               rate == 16384.0 || rate == 262144.0;
        CHECK(plausible);
        rates.push_back(rate);
    }
    REQUIRE_FALSE(rates.empty());

    // Two is the smallest number that makes "all of them" mean anything. It is
    // still a weak threshold, and deliberately so: the check exists to catch a
    // whole corpus collapsing to one value, and raising the bar further would
    // switch it off for most collections that are not the 783-file one it was
    // written against.
    constexpr std::size_t kEnoughGamesToTell = 2;
    if (rates.size() < kEnoughGamesToTell) {
        SUCCEED("one game is not a corpus: the reset-default check needs a sample");
        return;
    }

    constexpr double kResetDefault = 32768.0;
    const bool anyPastReset =
        std::any_of(rates.begin(), rates.end(),
                    [](double rate) { return rate != kResetDefault; });
    INFO("every sampled game reported the 32768 Hz reset default, which is what a "
         "probe that stops before the game configures SOUNDBIAS returns");
    CHECK(anyPastReset);
}

TEST_CASE("a GSF honours the length and fade tags", "[gsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto gsfs = findGsfs(6);
    if (gsfs.empty()) {
        SKIP("corpus holds no GSF files");
    }

    // PSF has no intrinsic duration, so the decoder is what makes the track
    // finite -- and the reported length has to include the fade, or playback
    // stops partway through it.
    int checked = 0;
    for (const fs::path& path : gsfs) {
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

TEST_CASE("a GSF fades out rather than being cut off", "[gsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // A short track that states its own fade, so the whole thing can be decoded.
    for (const fs::path& path : findGsfs(8)) {
        const auto tags = readPsfTags(Url::fromLocalPath(path), registry());
        if (!tags || !tags->length || *tags->length <= 0.0 || *tags->length > 60.0 ||
            !tags->fade || *tags->fade < 1.0) {
            continue;
        }
        INFO(path.filename().string());

        const Decoded decoded = decode(path, std::size_t{1} << 30);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.frames() == static_cast<std::size_t>(decoded.properties.totalFrames));

        const auto rate = static_cast<std::size_t>(decoded.properties.format.sampleRate);
        const auto fadeStart =
            static_cast<std::size_t>(*tags->length * static_cast<double>(rate));
        REQUIRE(fadeStart < decoded.frames());

        const int before = peak(decoded.samples, fadeStart - rate / 2, fadeStart);
        const int after =
            peak(decoded.samples, decoded.frames() - rate / 20, decoded.frames());
        INFO("peak before the fade " << before << ", at the end " << after);
        CHECK(before > 500);
        CHECK(after * 10 < before);
        return;
    }
    SKIP("corpus holds no short GSF that states a fade");
}

TEST_CASE("the GSF core refuses another console's PSF", "[gsf][corpus]") {
    if (!psfCorpusPresent()) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findPsfFiles({".usf", ".miniusf"}, {.want = 1});
    if (usfs.empty()) {
        SKIP("corpus holds no USF to check the version byte against");
    }

    // A USF handed to a GBA is not a near miss. The container refuses it on the
    // version byte rather than mGBA discovering it by executing a save state.
    INFO(usfs.front().filename().string());
    const Url url = Url::fromLocalPath(usfs.front());
    CHECK(loadPsf(url, registry()).has_value());
    CHECK_FALSE(loadPsf(url, registry(), 0x22).has_value());
}
