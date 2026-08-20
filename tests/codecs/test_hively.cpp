// AHX and Hively tracker modules.
//
// Two things carry their weight here and the rest is scaffolding.
//
// The first is that `.ahx` reaches this decoder at all. vgmstream claims the
// same extension for CRI's unrelated format and both sit at the default
// priority, so which one plays an AHX is decided by content: vgmstream is
// offered it, declines, and the registry moves on. That is a property of the
// whole registry rather than of either codec, and it is exactly the sort of
// thing that works until someone gives one of them a priority.
//
// The second is the container. Most AHX files that declare subsongs declare
// ones that do not exist -- entries pointing past the end of the position list,
// which the replayer clamps to zero -- and expanding on the declared count
// alone fills a playlist with the same tune under different numbers. The test
// for that is not "the count is right" but "the rows sound different".
//
// Modules cannot be committed, so the corpus cases run against a collection
// already on the machine (`-DXPCOG_HIVELY_CORPUS=<path>`) and skip without one.
// What does not need a corpus -- registration, and refusing things that are not
// modules -- runs everywhere.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;
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

#ifdef XPCOG_HIVELY_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_HIVELY_CORPUS}; }
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

/// Up to `want` modules, of either format.
[[nodiscard]] std::vector<fs::path> findModules(std::size_t want,
                                                std::string_view only = {}) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }

    std::error_code                  error;
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
        if (extension != ".ahx" && extension != ".hvl") {
            continue;
        }
        if (!only.empty() && extension != only) {
            continue;
        }
        found.push_back(entry.path());
    }
    return found;
}

struct Decoded {
    std::vector<float> samples;
    TrackProperties    properties;
};

[[nodiscard]] Decoded decode(const Url& url, std::size_t limitSamples) {
    Decoded                    out;
    PluginRegistry::OpenResult opened = registry().open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();

    AudioChunk chunk;
    while (out.samples.size() < limitSamples && opened.decoder->readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t channels = chunk.format().channels;
        const std::size_t at       = out.samples.size();
        out.samples.resize(at + frames * channels);
        std::memcpy(out.samples.data() + at, chunk.bytes().data(),
                    frames * channels * sizeof(float));
    }
    return out;
}

[[nodiscard]] float peak(const std::vector<float>& samples) {
    float highest = 0.0F;
    for (const float sample : samples) {
        highest = std::max(highest, std::abs(sample));
    }
    return highest;
}

/// Writes `bytes` to a temporary file named `name` and returns its URL. For the
/// cases that need a file the registry will offer to this decoder by extension
/// but which is not a module.
[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path dir = fs::temp_directory_path() / "xpcog-hively-tests";
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path path = dir / name;

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
    return Url::fromLocalPath(path);
}

constexpr std::size_t kTenSeconds = 441000 * 2;

}  // namespace

TEST_CASE("the Hively decoder claims both of its formats", "[hively]") {
    CHECK(registry().isPlayableExtension("ahx"));
    CHECK(registry().isPlayableExtension("hvl"));
}

TEST_CASE("something that is not a module is declined", "[hively]") {
    // The extension gets the file offered to this decoder; the header is what
    // decides. Worth pinning because the replayer's own check is the thing
    // resolving the `.ahx` conflict with vgmstream -- a decoder that accepted
    // whatever it was handed would take CRI's files away from vgmstream and
    // play them as noise, which is the same fault in the other direction.
    CHECK_FALSE(registry().open(writeTemp("garbage.ahx", "not a tracker module")));
    CHECK_FALSE(registry().open(writeTemp("empty.hvl", "")));

    // A header that starts right and stops early. hvl_LoadTune reads its sizes
    // out of the first fourteen bytes, so a file shorter than that is where a
    // trusting loader reads past the end.
    CHECK_FALSE(registry().open(writeTemp("truncated.ahx", std::string("THX\0", 4))));
}

TEST_CASE("a module renders audio, not silence", "[hively][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_HIVELY_CORPUS=<path> to run this");
    }
    const auto modules = findModules(8);
    if (modules.empty()) {
        SKIP("corpus holds no AHX or HVL files");
    }

    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        const Decoded decoded = decode(Url::fromLocalPath(path), kTenSeconds);
        REQUIRE_FALSE(decoded.samples.empty());

        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.format == SampleFormat::F32);
        CHECK(decoded.properties.seekable);

        // A replayer that loaded the tune and then ran no voices emits perfect
        // silence, so the level is what separates "it parsed" from "it played".
        //
        // Both bounds are loose on purpose, because what they are checking is
        // the *divisor* and not the tune. The replayer's int32 is full-scale at
        // 1 << 24 rather than 1 << 31, and getting that wrong is wrong by a
        // factor of 128 -- inaudible silence one way, a wall of clipping the
        // other. Neither bound says anything about how loud a given module is,
        // and deliberately: over a forty-module sample the median peak is 0.52
        // and three of them pass 1.0, the loudest at 1.82. The mix gain is a
        // per-file field (hvl_replay.c line 649, and a table indexed by the
        // stereo setting for AHX), so a hot module is the author's doing and
        // clamping it here would only throw away headroom the float format is
        // there to carry. Cog divides by the same number and lets the same
        // modules exceed unity.
        const float level = peak(decoded.samples);
        INFO("peak " << level);
        CHECK(level > 0.01F);
        CHECK(level < 8.0F);
    }
}

TEST_CASE("each format is named as itself", "[hively][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_HIVELY_CORPUS=<path> to run this");
    }

    // One replayer reads both, and it records nothing about which loader ran --
    // so the decoder re-reads the magic itself, and this is what says it read it
    // rather than guessing from the extension it was opened under.
    const auto ahx = findModules(1, ".ahx");
    const auto hvl = findModules(1, ".hvl");
    if (ahx.empty() || hvl.empty()) {
        SKIP("corpus holds no pair of AHX and HVL files");
    }

    PluginRegistry::OpenResult first = registry().open(Url::fromLocalPath(ahx.front()));
    REQUIRE(first);
    CHECK(first.decoder->properties().codec == "Abyss' Highest eXperience");

    PluginRegistry::OpenResult second = registry().open(Url::fromLocalPath(hvl.front()));
    REQUIRE(second);
    CHECK(second.decoder->properties().codec == "Hively Tracker");
}

TEST_CASE("a module has a length and a title", "[hively][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_HIVELY_CORPUS=<path> to run this");
    }
    const auto modules = findModules(20);
    if (modules.empty()) {
        SKIP("corpus holds no AHX or HVL files");
    }

    bool titled = false;
    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        // The length is measured by playing the tune through, so a zero here is
        // the measurement having gone wrong rather than a short module. The
        // upper bound is the safety cut-out in measurePlayingTime(): a tune that
        // hit it would report two hours per pass, which is the runaway that
        // bound exists to stop and is worth noticing rather than shipping.
        const std::int64_t frames = opened.decoder->properties().totalFrames;
        CHECK(frames > 0);
        CHECK(frames < static_cast<std::int64_t>(44100) * 3600);

        if (!opened.decoder->metadata().first("title").empty()) {
            titled = true;
        }
    }

    // Not asserted per file: ht_Name is a free-text field and plenty of authors
    // left it blank. That any of them survived is what says the field is being
    // read and trimmed rather than dropped.
    CHECK(titled);
}

TEST_CASE("only subsongs that differ become playlist entries", "[hively][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_HIVELY_CORPUS=<path> to run this");
    }
    const auto modules = findModules(400);
    if (modules.empty()) {
        SKIP("corpus holds no AHX or HVL files");
    }

    bool testedMulti  = false;
    bool testedSingle = false;

    for (const fs::path& path : modules) {
        const std::vector<Url> songs =
            registry().expandContainer(Url::fromLocalPath(path));

        if (songs.size() < 2) {
            // A tune with nothing to expand stays exactly one row, and that row
            // is the file itself rather than `file.ahx#0` -- a fragment on a
            // single-song module is noise in a playlist.
            REQUIRE(songs.size() == 1);
            CHECK(songs.front().fragment().empty());
            testedSingle = true;
            continue;
        }

        INFO(path.filename().string());

        // Numbered from zero, because the replayer's subsong 0 *is* the main
        // song rather than a duplicate of it -- the opposite of libsidplayfp,
        // where song 0 means "the tune's default" and counting from zero loses
        // the last song.
        CHECK(songs.front().fragment() == "0");

        // The decisive one. Every row here claims to be a different song, and
        // the reason the container reads the subsong table rather than trusting
        // ht_SubsongNr is that most files declaring subsongs declare ones that
        // resolve to position zero -- which play the main song again. Rendering
        // two rows and comparing is what tells those apart; counting them is
        // not, and counting them passes either way.
        const Decoded first = decode(songs.front(), kTenSeconds / 4);
        const Decoded other = decode(songs.back(), kTenSeconds / 4);
        REQUIRE_FALSE(first.samples.empty());
        REQUIRE_FALSE(other.samples.empty());

        const std::size_t common = std::min(first.samples.size(), other.samples.size());
        REQUIRE(common > 0);
        CHECK_FALSE(std::equal(first.samples.begin(), first.samples.begin() +
                                                         static_cast<std::ptrdiff_t>(common),
                               other.samples.begin()));
        testedMulti = true;
        break;
    }

    CHECK(testedSingle);
    if (!testedMulti) {
        SKIP("corpus holds no module with a subsong that starts anywhere else");
    }
}

TEST_CASE("seeking lands where it was asked to", "[hively][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_HIVELY_CORPUS=<path> to run this");
    }
    const auto modules = findModules(1);
    if (modules.empty()) {
        SKIP("corpus holds no AHX or HVL files");
    }

    PluginRegistry::OpenResult opened =
        registry().open(Url::fromLocalPath(modules.front()));
    REQUIRE(opened);

    // A tracker has no random access -- the state at any moment is every note
    // and effect before it -- so seeking is replaying, and what can be checked
    // is that it stops where it was told rather than somewhere near.
    constexpr std::int64_t kTarget = 44100 * 5;
    if (opened.decoder->properties().totalFrames <= kTarget) {
        SKIP("first module is shorter than the seek target");
    }
    CHECK(opened.decoder->seek(kTarget) == kTarget);

    // And that it can go backwards, which means starting the subsong again.
    CHECK(opened.decoder->seek(0) == 0);

    AudioChunk chunk;
    CHECK(opened.decoder->readAudio(chunk));
    CHECK(chunk.frameCount() > 0);
}
