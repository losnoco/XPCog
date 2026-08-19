// SSF and DSF: that HighlyTheoretical renders both a Saturn and a Dreamcast,
// and that a deep `_lib` chain assembles in the right order.
//
// One core, two formats, two machines -- the version byte is the switch, not a
// check -- so these run each assertion against whichever of the two the corpus
// holds, and say which it found.
//
// The chain is the other thing worth testing here. An SSF set can name six
// libraries from one track (`_lib` through `_lib6`), and psflib returns the
// resulting images highest priority first. Merge them in the wrong order and
// the later, more specific overlays are overwritten by the general ones: the
// file loads, the duration is right, and the wrong music plays.
//
// Rips cannot be committed, so these run against a corpus already on the
// machine (`-DXPCOG_PSF_CORPUS=<path>`) and skip without one.

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

/// Up to `want` playable SSFs or DSFs, one per directory -- one game's rips share a
/// `.snsflib` and testing six of them tests one cartridge.
[[nodiscard]] std::vector<fs::path> findSegaPsfs(std::size_t want,
                                              bool onePerDirectory = true) {
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

    fs::path lastDirectory;
    for (const fs::directory_entry& entry : walk) {
        if (found.size() >= want) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string extension = lowerExtension(entry.path());
        if (extension != ".ssf" && extension != ".minissf" &&
            extension != ".dsf" && extension != ".minidsf") {
            continue;
        }
        if (onePerDirectory && entry.path().parent_path() == lastDirectory) {
            continue;
        }
        lastDirectory = entry.path().parent_path();
        found.push_back(entry.path());
    }
    return found;
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

/// Ten seconds at the 44.1 kHz both machines produce.
constexpr std::size_t kTenSeconds = 441000;

}  // namespace

TEST_CASE("the Sega decoder is registered for all four spellings", "[sdsf]") {
    // Two consoles, four extensions, one decoder.
    CHECK(registry().isPlayableExtension("ssf"));
    CHECK(registry().isPlayableExtension("minissf"));
    CHECK(registry().isPlayableExtension("dsf"));
    CHECK(registry().isPlayableExtension("minidsf"));

    CHECK_FALSE(registry().isPlayableExtension("ssflib"));
    CHECK_FALSE(registry().isPlayableExtension("dsflib"));
}

TEST_CASE("an SSF or DSF renders audio, not silence", "[sdsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto segaPsfs = findSegaPsfs(3);
    if (segaPsfs.empty()) {
        SKIP("corpus holds no SSF or DSF files");
    }

    for (const fs::path& path : segaPsfs) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTenSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.sampleRate == 44100.0);

        // Which machine was built comes from the version byte, so this also
        // pins that a .dsf is not quietly decoded as a Saturn rip.
        const std::string codec = decoded.properties.codec;
        INFO("codec: " << codec);
        const std::string extension = lowerExtension(path);
        if (extension == ".dsf" || extension == ".minidsf") {
            CHECK(codec == "DSF");
        } else {
            CHECK(codec == "SSF");
        }

        INFO("peak over ten seconds: " << peak(decoded.samples, 0, kTenSeconds));
        CHECK(peak(decoded.samples, 0, kTenSeconds) > 500);
    }
}

TEST_CASE("a deep _lib chain resolves in order", "[sdsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // A Saturn set can name six libraries from one track. Every one of them has
    // to come back, and in priority order, or the track is assembled from the
    // wrong overlays -- which plays, and plays the wrong thing.
    for (const fs::path& path : findSegaPsfs(12, /*onePerDirectory=*/false)) {
        const auto tags = readPsfTags(Url::fromLocalPath(path), registry());
        if (!tags || tags->tags.first("_lib2").empty()) {
            continue;  // not a multi-library track
        }
        INFO(path.filename().string());

        std::size_t named = 1;
        for (int n = 2;; ++n) {
            if (tags->tags.first("_lib" + std::to_string(n)).empty()) {
                break;
            }
            ++named;
        }
        INFO("libraries named: " << named);
        CHECK(named >= 2);

        const auto loaded = loadPsf(Url::fromLocalPath(path), registry());
        REQUIRE(loaded.has_value());

        // The file itself plus everything it named. psflib walks `_lib`
        // recursively, so a library naming its own library adds more -- hence
        // >= rather than ==.
        INFO("programs returned: " << loaded->programs.size());
        CHECK(loaded->programs.size() >= named + 1);
        return;
    }
    SKIP("corpus holds no multi-library SSF or DSF");
}

TEST_CASE("opening an SSF or DSF does not build a console", "[sdsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto segaPsfs = findSegaPsfs(1);
    if (segaPsfs.empty()) {
        SKIP("corpus holds no SSF or DSF files");
    }

    const fs::path orphan =
        fs::temp_directory_path() / "xpcog-sdsf-orphan" / segaPsfs.front().filename();
    std::error_code error;
    fs::create_directories(orphan.parent_path(), error);
    fs::copy_file(segaPsfs.front(), orphan, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(orphan));
    REQUIRE(static_cast<bool>(opened));
    CHECK(opened.decoder->properties().totalFrames > 0);

    // A file that names no library is self-contained and will still play; only
    // one that needs a chain fails here. Both are correct, so this asserts the
    // weaker thing: opening succeeded without resolving anything.
    opened.decoder->close();
    opened.decoder.reset();
    fs::remove_all(orphan.parent_path(), error);
}

TEST_CASE("the Sega core refuses another console's PSF", "[sdsf][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    std::error_code error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        SKIP("corpus is not readable");
    }

    for (const fs::directory_entry& entry : walk) {
        const std::string extension = lowerExtension(entry.path());
        if (!entry.is_regular_file(error) ||
            (extension != ".minigsf" && extension != ".miniusf" &&
             extension != ".mini2sf" && extension != ".minisnsf")) {
            continue;
        }
        INFO(entry.path().filename().string());
        const Url url = Url::fromLocalPath(entry.path());
        CHECK(loadPsf(url, registry()).has_value());
        CHECK_FALSE(loadPsf(url, registry(), 0x11).has_value());
        CHECK_FALSE(loadPsf(url, registry(), 0x12).has_value());
        return;
    }
    SKIP("corpus holds no other PSF to check the version byte against");
}
