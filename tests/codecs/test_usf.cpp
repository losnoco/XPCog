// USF: that lazyusf2 actually renders a Nintendo 64's audio, and that the
// track it produces begins and ends where the tags say.
//
// These decode. That is the point of them -- the container tests next door in
// test_psf.cpp prove the `_lib` chain resolves, which a USF core can satisfy
// completely and still play silence, because a USF keeps its save state in the
// `reserved` section while every other PSF variant uses `exe`. Nothing short of
// looking at the samples catches that.
//
// Rips cannot be committed, so these run against a corpus already on the
// machine (`-DXPCOG_PSF_CORPUS=<path>`) and skip without one. They find files by
// extension and assert on relationships -- length against the tag, loudness
// against silence -- never on a particular game.

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
namespace fs = std::filesystem;

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

#ifdef XPCOG_PSF_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_PSF_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

[[nodiscard]] std::string lowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// Up to `want` playable USFs. `.usflib` is skipped deliberately: it is the
/// game's program with no track in it, and no decoder claims it.
[[nodiscard]] std::vector<fs::path> findUsfs(std::size_t want) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }

    std::error_code error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }

    for (const fs::directory_entry& entry : walk) {
        if (found.size() >= want) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string extension = lowerExtension(entry.path());
        if (extension == ".usf" || extension == ".miniusf") {
            found.push_back(entry.path());
        }
    }
    return found;
}

/// The first USF in the corpus that states its own fade, or an empty path. Most
/// rips state only a length, so the fade cases have to go looking.
[[nodiscard]] fs::path findFadedUsf() {
    for (const fs::path& path : findUsfs(60)) {
        const auto tags = readPsfTags(Url::fromLocalPath(path), registry());
        if (tags && tags->length && *tags->length > 0.0 && *tags->length < 120.0 &&
            tags->fade && *tags->fade > 1.0) {
            return path;
        }
    }
    return {};
}

/// Decoded interleaved samples, stopping at `limit` frames.
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

/// Two seconds is plenty to tell music from silence and keeps a corpus-wide
/// sweep out of these tests; the sweep is a developer's job, not CI's.
constexpr std::size_t kTwoSeconds = 88200;

}  // namespace

TEST_CASE("the USF decoder is registered for both spellings", "[usf]") {
    CHECK(registry().isPlayableExtension("usf"));
    CHECK(registry().isPlayableExtension("miniusf"));

    // Not `usflib`. It holds the game's whole program and no track: claiming it
    // would put one unplayable row in the playlist for every set scanned.
    CHECK_FALSE(registry().isPlayableExtension("usflib"));
}

TEST_CASE("a USF renders audio, not silence", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findUsfs(3);
    if (usfs.empty()) {
        SKIP("corpus holds no USF files");
    }

    for (const fs::path& path : usfs) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTwoSeconds);
        REQUIRE(decoded.frames() > 0);

        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.format.channels == 2);

        // And that it was *this* decoder. vgmstream claims `usf` and `miniusf`
        // among its several hundred extensions; it is registered below
        // kDefaultPriority so the dedicated core is tried first, and if that
        // ordering ever slipped these files would still open -- through the
        // wrong decoder, with different metadata and no `_lib` chain.
        CHECK(decoded.properties.codec == "USF");

        // The whole reason this file exists. A core that reads `exe` instead of
        // `reserved` boots an empty machine: the chain resolves, the tags are
        // right, the duration is right, and every sample is zero. Anything above
        // a few hundred is unambiguously an N64 making noise.
        INFO("peak over the first two seconds: " << peak(decoded.samples, 0, kTwoSeconds));
        CHECK(peak(decoded.samples, 0, kTwoSeconds) > 500);
    }
}

TEST_CASE("a USF starts at the first sound, not at the save state", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findUsfs(3);
    if (usfs.empty()) {
        SKIP("corpus holds no USF files");
    }

    // A save state is captured a little before the music does anything, and how
    // much slack varies by rip. Untrimmed, every track of a set opens with dead
    // air of a different length and `length` measures from the wrong instant.
    // A tenth of a second in, there should already be sound.
    for (const fs::path& path : usfs) {
        INFO(path.filename().string());
        const Decoded decoded = decode(path, 4410);
        REQUIRE(decoded.frames() >= 4410);
        CHECK(peak(decoded.samples, 0, 4410) > 100);
    }
}

TEST_CASE("the length tag is the length of the track", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findUsfs(6);
    if (usfs.empty()) {
        SKIP("corpus holds no USF files");
    }

    // PSF has no intrinsic duration -- the program would run for ever -- so the
    // tag is the only thing that makes the track finite, and the decoder has to
    // be the one that stops. Reported frames must be length plus fade, since
    // playback cut off at `length` would clip the fade it just started.
    int checked = 0;
    for (const fs::path& path : usfs) {
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

TEST_CASE("a stated fade actually fades", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const fs::path path = findFadedUsf();
    if (path.empty()) {
        SKIP("corpus holds no short USF that states a fade");
    }
    INFO(path.filename().string());

    const auto tags = readPsfTags(Url::fromLocalPath(path), registry());
    REQUIRE(tags);

    // Decoded whole, which is why findFadedUsf() insists on a short one.
    const Decoded decoded = decode(path, std::size_t{1} << 30);
    REQUIRE(decoded.frames() > 0);
    CHECK(decoded.frames() == static_cast<std::size_t>(decoded.properties.totalFrames));

    const auto rate      = static_cast<std::size_t>(decoded.properties.format.sampleRate);
    const auto fadeStart = static_cast<std::size_t>(*tags->length * static_cast<double>(rate));
    REQUIRE(fadeStart < decoded.frames());

    // Loud before the fade begins, and gone by the end of it. Without the fade
    // the track would simply be cut off at full volume, which is what the
    // comparison between these two numbers detects.
    const int before = peak(decoded.samples, fadeStart - rate / 2, fadeStart);
    const int after  = peak(decoded.samples, decoded.frames() - rate / 10, decoded.frames());
    INFO("peak before the fade " << before << ", at the end " << after);
    CHECK(before > 500);
    CHECK(after * 20 < before);
}

TEST_CASE("seeking lands on the same audio as playing through", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findUsfs(1);
    if (usfs.empty()) {
        SKIP("corpus holds no USF files");
    }
    INFO(usfs.front().filename().string());

    // There is no rewinding an emulator: seeking restarts the machine and runs
    // it forward in silence. That only lands in the right place if emulation is
    // deterministic *and* the leading silence is trimmed identically the second
    // time -- get the second wrong and every seek is off by the trim, which
    // sounds like nothing worse than a slightly early cue.
    constexpr std::int64_t kTarget = 44100;

    const Decoded straight = decode(usfs.front(), kTarget + kTwoSeconds);
    REQUIRE(straight.frames() > static_cast<std::size_t>(kTarget) + 4410);

    const Decoded sought = decode(usfs.front(), 4410, kTarget);
    REQUIRE(sought.frames() >= 4410);

    const auto* expected = straight.samples.data() + kTarget * 2;
    CHECK(std::equal(sought.samples.begin(), sought.samples.begin() + 4410 * 2, expected));
}

TEST_CASE("opening a USF does not boot an N64", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto usfs = findUsfs(1);
    if (usfs.empty()) {
        SKIP("corpus holds no USF files");
    }

    // Scanner::readMetadata opens a decoder for every file it walks, only to
    // ask for properties -- and every one of those answers comes from the tag
    // block. So open() reads tags and stops, and the emulator waits for the
    // first frame anyone wants. Cog does the same.
    //
    // "No emulator was started" is otherwise only observable with a stopwatch,
    // and a timing assertion is not a test. This is the behavioural
    // consequence instead: the tags-only path stops before psflib follows
    // `_lib`, so a mini-PSF carried away from its library still opens -- and
    // then has nothing to play. Eager loading could not produce that, because
    // it resolves the chain up front and fails there.
    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-usf-orphan" / usfs.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(usfs.front(), orphan, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(orphan));
    REQUIRE(static_cast<bool>(opened));

    // Tags and duration, with no library in sight.
    CHECK(opened.decoder->properties().totalFrames > 0);

    AudioChunk chunk;
    CHECK_FALSE(opened.decoder->readAudio(chunk));

    opened.decoder->close();
    opened.decoder.reset();
    fs::remove_all(orphan.parent_path(), error);
}

TEST_CASE("the USF core refuses another console's PSF", "[usf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // The version byte says which emulator a program image is for. Feeding a
    // GBA image to an N64 is not a near miss -- it is arbitrary bytes at the
    // reset vector -- so the container refuses it rather than the core
    // discovering it by executing garbage.
    std::error_code error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        SKIP("corpus is not readable");
    }

    for (const fs::directory_entry& entry : walk) {
        if (!entry.is_regular_file(error) || lowerExtension(entry.path()) != ".minigsf") {
            continue;
        }
        INFO(entry.path().filename().string());
        const Url url = Url::fromLocalPath(entry.path());

        // Loadable as a PSF, and not loadable as a USF.
        CHECK(loadPsf(url, registry()).has_value());
        CHECK_FALSE(loadPsf(url, registry(), 0x21).has_value());
        return;
    }
    SKIP("corpus holds no GSF to check the version byte against");
}
