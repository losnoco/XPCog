// QSF: that HighlyQuixotic renders a CPS-2 sound board, and -- the point of the
// format -- that a miniqsf's one byte of song select survives the merge.
//
// A QSF chain is unlike the others in what it carries. There is no executable
// and no save state: the library holds two ROM images, the Z80 sound program
// and the PCM samples, and a miniqsf usually holds a single byte written over
// the Z80 image at a fixed offset. That byte is the track number.
//
// Which makes the ordering rule load-bearing in a way it is nowhere else. Apply
// the chain backwards and every file in the set still opens, still reports its
// tags and its duration, and still plays -- the same track, sixty times over,
// because the library's own default overwrote the selection instead of the
// other way round. So the test that matters here is not that a QSF makes sound
// but that two different QSFs make *different* sound.
//
// Rips cannot be committed, so these run against a corpus already on the
// machine (`-DXPCOG_PSF_CORPUS=<path>`) and skip without one.

#include "psf/PsfFile.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
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

/// Every QSF in the corpus, grouped by the folder it sits in.
///
/// Grouped rather than flat because the interesting case needs two rips that
/// share a library, and a folder is what a set is.
[[nodiscard]] std::map<fs::path, std::vector<fs::path>> findQsfSets() {
    std::map<fs::path, std::vector<fs::path>> sets;
    if (!kHaveCorpus) {
        return sets;
    }

    std::error_code error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return sets;
    }

    for (const fs::directory_entry& entry : walk) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string extension = lowerExtension(entry.path());
        if (extension != ".qsf" && extension != ".miniqsf") {
            continue;
        }
        sets[entry.path().parent_path()].push_back(entry.path());
    }
    for (auto& [directory, files] : sets) {
        (void)directory;
        std::sort(files.begin(), files.end());
    }
    return sets;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
    [[nodiscard]] std::size_t frames() const { return samples.size() / 2; }
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

[[nodiscard]] int peak(const std::vector<std::int16_t>& samples) {
    int highest = 0;
    for (const std::int16_t sample : samples) {
        highest = std::max(highest, std::abs(static_cast<int>(sample)));
    }
    return highest;
}

/// Ten seconds at the QSound DSP's own rate.
constexpr std::size_t kSampleRate = 24038;
constexpr std::size_t kTenSeconds = kSampleRate * 10;

}  // namespace

TEST_CASE("the QSF decoder is registered for both spellings", "[qsf]") {
    CHECK(registry().isPlayableExtension("qsf"));
    CHECK(registry().isPlayableExtension("miniqsf"));

    // The library is a container, not a track: it holds the ROMs every rip in
    // the set shares and selects nothing.
    CHECK_FALSE(registry().isPlayableExtension("qsflib"));
}

TEST_CASE("a QSF renders audio, not silence", "[qsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto sets = findQsfSets();
    if (sets.empty()) {
        SKIP("corpus holds no QSF files");
    }

    for (const auto& [directory, files] : sets) {
        INFO(directory.filename().string());
        REQUIRE_FALSE(files.empty());

        const Decoded decoded = decode(files.front(), kTenSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.sampleRate ==
              static_cast<double>(kSampleRate));
        CHECK(decoded.properties.codec == "QSF");

        // A CPS-2 board with no ROM in it renders digital silence perfectly
        // well, so a level check is what separates "the core ran" from "the
        // core ran and had something to play".
        CHECK(peak(decoded.samples) > 500);
    }
}

TEST_CASE("two QSFs from one library play different tracks", "[qsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto sets = findQsfSets();
    if (sets.empty()) {
        SKIP("corpus holds no QSF files");
    }

    bool tested = false;
    for (const auto& [directory, files] : sets) {
        if (files.size() < 2) {
            continue;
        }
        INFO(directory.filename().string());

        // The last file rather than the second: adjacent track numbers in an
        // arcade set are often two cuts of the same cue.
        const Decoded first = decode(files.front(), kTenSeconds);
        const Decoded other = decode(files.back(), kTenSeconds);
        REQUIRE(first.frames() > 0);
        REQUIRE(other.frames() > 0);

        // Both drew on the same two ROMs and differ only in the byte the
        // miniqsf wrote over the Z80 image. If the merge applied the library
        // last, that byte is gone and these two buffers are identical.
        const std::size_t common = std::min(first.samples.size(), other.samples.size());
        REQUIRE(common > 0);
        CHECK_FALSE(std::equal(first.samples.begin(), first.samples.begin() +
                                                          static_cast<std::ptrdiff_t>(common),
                               other.samples.begin()));
        tested = true;
    }

    if (!tested) {
        SKIP("corpus holds no QSF set with two or more tracks");
    }
}

TEST_CASE("the QSF core refuses another console's PSF", "[qsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto sets = findQsfSets();
    if (sets.empty()) {
        SKIP("corpus holds no QSF files");
    }

    // Every core in this tree is handed files by extension, so the version
    // check is the only thing standing between a mislabelled rip and a Z80
    // executing a PlayStation executable.
    const fs::path qsf = sets.begin()->second.front();
    CHECK(loadPsf(Url::fromLocalPath(qsf), registry(), 0x41).has_value());
    CHECK_FALSE(loadPsf(Url::fromLocalPath(qsf), registry(), 0x01).has_value());
    CHECK_FALSE(loadPsf(Url::fromLocalPath(qsf), registry(), 0x22).has_value());
}

TEST_CASE("seeking a QSF lands on the frame it was asked for", "[qsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto sets = findQsfSets();
    if (sets.empty()) {
        SKIP("corpus holds no QSF files");
    }

    const fs::path qsf = sets.begin()->second.front();
    INFO(qsf.filename().string());

    // A QSF has no seekable stream behind it -- seeking means running the Z80
    // from the top and throwing the samples away -- so what is being checked is
    // that the discard loop counts what the core actually produced rather than
    // what it was asked for.
    const Decoded straight = decode(qsf, kTenSeconds);
    REQUIRE(straight.frames() >= kTenSeconds);

    const std::size_t target = kSampleRate * 5;
    const Decoded     sought = decode(qsf, kSampleRate, static_cast<std::int64_t>(target));
    REQUIRE(sought.frames() > 0);

    const std::size_t compare = std::min(sought.frames(), straight.frames() - target);
    REQUIRE(compare > 0);
    CHECK(std::equal(sought.samples.begin(),
                     sought.samples.begin() + static_cast<std::ptrdiff_t>(compare * 2),
                     straight.samples.begin() + static_cast<std::ptrdiff_t>(target * 2)));
}
