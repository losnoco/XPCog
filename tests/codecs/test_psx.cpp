// PSF and PSF2: that HighlyExperimental renders a PlayStation and a
// PlayStation 2, at the rate each one actually runs at.
//
// One core, two consoles, and the version byte is the switch:
// `psx_get_state_size(1)` builds a PS1 and `(2)` a PS2. The two formats also
// load completely differently -- a PSF carries a PS-EXE that is uploaded into
// IOP RAM, while a PSF2 carries no executable at all and its sections are a
// *filesystem* the running machine reads out of on demand. So the interesting
// assertions here are that the right machine is built and that both loading
// paths work.
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

/// Up to `want` playable PSFs or PSF2s, one per directory -- one game's rips share a
/// `.snsflib` and testing six of them tests one cartridge.
/// `only` restricts the search to one spelling. Without it the first `want`
/// files are whatever the directory walk reaches first, which in a corpus
/// holding both formats can easily be eight of the same one -- and a test
/// looking for the other then skips while believing the corpus lacks it.
[[nodiscard]] std::vector<fs::path> findPsx(std::size_t want,
                                            bool onePerDirectory  = true,
                                            std::string_view only = {}) {
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
        const bool isPs2 = extension == ".psf2" || extension == ".minipsf2";
        const bool isPs1 = extension == ".psf" || extension == ".minipsf";
        if (!isPs1 && !isPs2) {
            continue;
        }
        if (only == "psf2" && !isPs2) {
            continue;
        }
        if (only == "psf" && !isPs1) {
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

/// Ten seconds at the rate SSEQPlayer is asked to synthesise.
constexpr std::size_t kTenSeconds = 441000;

}  // namespace

TEST_CASE("the PlayStation decoder is registered for all four spellings", "[psx]") {
    CHECK(registry().isPlayableExtension("psf"));
    CHECK(registry().isPlayableExtension("minipsf"));
    CHECK(registry().isPlayableExtension("psf2"));
    CHECK(registry().isPlayableExtension("minipsf2"));

    CHECK_FALSE(registry().isPlayableExtension("psflib"));
    CHECK_FALSE(registry().isPlayableExtension("psf2lib"));
}

TEST_CASE("a PSF or PSF2 renders audio at its console's rate", "[psx][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto psxFiles = findPsx(4, /*onePerDirectory=*/false);
    if (psxFiles.empty()) {
        SKIP("corpus holds no PSF or PSF2 files");
    }

    for (const fs::path& path : psxFiles) {
        INFO(path.filename().string());

        const Decoded decoded = decode(path, kTenSeconds);
        REQUIRE(decoded.frames() > 0);
        CHECK(decoded.properties.format.channels == 2);

        // The PS1's SPU runs at 44.1 kHz and the PS2's SPU2 at 48 kHz, and the
        // version byte is what decides which machine was built. Reporting one
        // console's rate for the other's audio is a pitch error, and every
        // other measurement -- duration, peak, tags -- stays correct through it.
        const std::string extension = lowerExtension(path);
        const bool        isPs2 =
            extension == ".psf2" || extension == ".minipsf2";
        INFO("codec " << decoded.properties.codec << " at "
                      << decoded.properties.format.sampleRate);
        if (isPs2) {
            CHECK(decoded.properties.codec == "PSF2");
            CHECK(decoded.properties.format.sampleRate == 48000.0);
        } else {
            CHECK(decoded.properties.codec == "PSF");
            CHECK(decoded.properties.format.sampleRate == 44100.0);
        }

        INFO("peak over ten seconds: " << peak(decoded.samples, 0, kTenSeconds));
        CHECK(peak(decoded.samples, 0, kTenSeconds) > 200);
    }
}

TEST_CASE("a PSF2 carries a filesystem rather than an executable", "[psx][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // The two formats load by different mechanisms and the container has to
    // hand both over intact. A PSF's `exe` is a PS-EXE, which begins with the
    // eight-byte "PS-X EXE" key; a PSF2's sections are directory and file
    // records with no such header, and psf2fs assembles them.
    for (const fs::path& path : findPsx(8, /*onePerDirectory=*/false, "psf2")) {
        const auto loaded = loadPsf(Url::fromLocalPath(path), registry(), 0x02);
        if (!loaded || loaded->programs.empty()) {
            continue;  // separated from its library; see the PSF case below
        }
        INFO(path.filename().string());

        bool anyExeHeader = false;
        for (const PsfProgram& program : loaded->programs) {
            if (program.exe.size() >= 8 &&
                std::memcmp(program.exe.data(), "PS-X EXE", 8) == 0) {
                anyExeHeader = true;
            }
        }
        CHECK_FALSE(anyExeHeader);
        return;
    }
    SKIP("corpus holds no PSF2 whose library chain resolves");
}

TEST_CASE("a PSF carries a PS-EXE", "[psx][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    // Candidates rather than the first hit: a corpus root can contain a
    // mini-PSF that has been separated from its library -- one such file sits
    // loose in the Downloads folder this was developed against -- and psflib
    // refuses those, correctly. readPsfTags() still succeeds on them, which is
    // the whole point of the tags-only path, so an unresolvable chain is a
    // property of the corpus rather than a failure to report.
    for (const fs::path& path : findPsx(8, /*onePerDirectory=*/false, "psf")) {
        const auto loaded = loadPsf(Url::fromLocalPath(path), registry(), 0x01);
        if (!loaded || loaded->programs.empty()) {
            continue;
        }
        INFO(path.filename().string());

        // The deepest library is returned first and is the one whose header
        // supplies the entry point, so it is the one that must look like a
        // PS-EXE. A 0x800-byte header is the format's minimum.
        const PsfProgram& first = loaded->programs.front();
        CHECK(first.exe.size() >= 0x800);
        CHECK(std::memcmp(first.exe.data(), "PS-X EXE", 8) == 0);
        return;
    }
    SKIP("corpus holds no PSF whose library chain resolves");
}

TEST_CASE("a PSF honours the length and fade tags", "[psx][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_PSF_CORPUS=<path> to run this");
    }

    const auto psxFiles = findPsx(6, /*onePerDirectory=*/false);
    if (psxFiles.empty()) {
        SKIP("corpus holds no PSF or PSF2 files");
    }

    int checked = 0;
    for (const fs::path& path : psxFiles) {
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

TEST_CASE("the PlayStation core refuses another console's PSF", "[psx][corpus]") {
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
             extension != ".mini2sf" && extension != ".minisnsf" &&
             extension != ".minincsf" && extension != ".dsf")) {
            continue;
        }
        INFO(entry.path().filename().string());
        const Url url = Url::fromLocalPath(entry.path());
        CHECK(loadPsf(url, registry()).has_value());
        CHECK_FALSE(loadPsf(url, registry(), 0x01).has_value());
        CHECK_FALSE(loadPsf(url, registry(), 0x02).has_value());
        return;
    }
    SKIP("corpus holds no other PSF to check the version byte against");
}
