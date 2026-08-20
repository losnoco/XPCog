// Syntrax modules (.jxs).
//
// The replayer is Cog's, verbatim, so what is under test here is the wrapper
// around it -- and the wrapper is where all three of the differences from Cog
// live.
//
// The one that carries the most weight is that **seeking is exact**: the
// decoder renders the gap and throws it away rather than using the replayer's
// null-buffer fast path, because that path advances the sequencer without
// running the mixer, and the mixer is where voice sample positions, synth
// phase, the echo delay lines and the declick overlap buffer are kept. If the
// gap is really being rendered, audio after a seek is bit-identical to the same
// audio decoded straight through -- and it stays identical even though the seek
// leaves the chunk boundaries somewhere else, which is a second thing worth
// knowing about this replayer.
//
// The other two are the subsong clamp -- jaytrax_changeSubsong() lets a number
// equal to the subsong count through and reads past the end of the array, which
// a URL fragment can ask for -- and expansion only when there is more than one
// subsong.
//
// Modules cannot be committed, so the corpus cases run against a collection
// already on the machine (`-DXPCOG_SYNTRAX_CORPUS=<path>`) and skip without
// one. Registration and rejection run everywhere.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

#ifdef XPCOG_SYNTRAX_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_SYNTRAX_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

[[nodiscard]] std::vector<fs::path> findModules(std::size_t want) {
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
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".jxs") {
            found.push_back(entry.path());
        }
    }
    // Sorted, so a failure names the same file on every machine that has it.
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] std::vector<std::int16_t> drain(IDecoder& decoder, std::size_t limitSamples) {
    std::vector<std::int16_t> samples;
    AudioChunk                chunk;
    while (samples.size() < limitSamples && decoder.readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t channels = chunk.format().channels;
        const std::size_t at       = samples.size();
        samples.resize(at + (frames * channels));
        std::memcpy(samples.data() + at, chunk.bytes().data(),
                    frames * channels * sizeof(std::int16_t));
    }
    return samples;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
};

[[nodiscard]] Decoded decode(const Url& url, std::size_t limitSamples) {
    Decoded                    out;
    PluginRegistry::OpenResult opened = registry().open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();
    out.samples    = drain(*opened.decoder, limitSamples);
    return out;
}

[[nodiscard]] int peak(const std::vector<std::int16_t>& samples) {
    int highest = 0;
    for (const std::int16_t sample : samples) {
        highest = std::max(highest, std::abs(static_cast<int>(sample)));
    }
    return highest;
}

[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path  dir = fs::temp_directory_path() / "xpcog-syntrax-tests";
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

TEST_CASE("the Syntrax decoder claims .jxs", "[syntrax]") {
    CHECK(registry().isPlayableExtension("jxs"));
}

TEST_CASE("something that is not a Syntrax module is declined", "[syntrax]") {
    CHECK_FALSE(registry().open(writeTemp("garbage.jxs", "not a tracker module")));
    CHECK_FALSE(registry().open(writeTemp("empty.jxs", "")));

    // A full-sized header whose version field is not 3456 or 3457. That field is
    // the format's only magic -- there is no signature string -- so this is the
    // check standing between the loader and a file whose pattern and instrument
    // counts are whatever happened to be in those bytes.
    CHECK_FALSE(registry().open(writeTemp("wrongversion.jxs", std::string(64, '\0'))));
}

TEST_CASE("a module renders audio, not silence", "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(8);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        const Decoded decoded = decode(Url::fromLocalPath(path), kTenSeconds);
        REQUIRE_FALSE(decoded.samples.empty());

        CHECK(decoded.properties.format.sampleRate == 44100.0);
        CHECK(decoded.properties.format.channels == 2);
        CHECK(decoded.properties.format.format == SampleFormat::S16);
        CHECK(decoded.properties.codec == "Syntrax");
        CHECK(decoded.properties.seekable);

        // The replayer clamps its own output to +/-32,760, so there is a real
        // ceiling here rather than a guess -- and the floor is what separates
        // "it parsed" from "it played".
        const int level = peak(decoded.samples);
        INFO("peak " << level);
        CHECK(level > 300);
        CHECK(level <= 32760);
    }
}

TEST_CASE("a module has a length and a title", "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(24);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    bool titled = false;
    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        // The length is measured by running the sequencer through, and the
        // replayer gives up after thirty minutes and answers -1. Cog adds the
        // fade to that and reports a negative length; here a module that hits
        // the cut-out falls back on the default track length instead -- so a
        // non-positive number means neither path ran.
        const std::int64_t frames = opened.decoder->properties().totalFrames;
        CHECK(frames > 0);
        CHECK(frames < static_cast<std::int64_t>(44100) * 3600);

        if (!opened.decoder->metadata().first("title").empty()) {
            titled = true;
        }
    }

    // Not asserted per file: the subsong name is free text and the tracker
    // writes "Empty" into unnamed ones, which is a title like any other. That
    // any survived is what says the field is read and trimmed rather than
    // dropped -- Cog notes some are entirely spaces.
    CHECK(titled);
}

TEST_CASE("only a module with subsongs becomes several playlist rows",
          "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(64);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    bool testedMulti  = false;
    bool testedSingle = false;

    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        const std::vector<Url> songs =
            registry().expandContainer(Url::fromLocalPath(path));

        if (songs.size() < 2) {
            // Cog expands unconditionally, so a one-subsong module -- which is
            // most of them -- becomes `something.jxs#0`: a fragment carrying no
            // information, on a playlist row that no longer matches the file
            // the listener dragged in.
            REQUIRE(songs.size() == 1);
            CHECK(songs.front().fragment().empty());
            testedSingle = true;
            continue;
        }

        // Numbered from zero, because subsong 0 is the module's first song
        // rather than a stand-in for "the default".
        CHECK(songs.front().fragment() == "0");

        // Counting rows says nothing about whether they play anything
        // different; rendering two of them does.
        const Decoded first = decode(songs.front(), kTenSeconds / 4);
        const Decoded other = decode(songs.back(), kTenSeconds / 4);
        REQUIRE_FALSE(first.samples.empty());
        REQUIRE_FALSE(other.samples.empty());

        const std::size_t common = std::min(first.samples.size(), other.samples.size());
        REQUIRE(common > 0);
        CHECK_FALSE(std::equal(
            first.samples.begin(),
            first.samples.begin() + static_cast<std::ptrdiff_t>(common),
            other.samples.begin()));
        testedMulti = true;

        if (testedSingle) {
            break;
        }
    }

    CHECK(testedSingle);
    if (!testedMulti) {
        SKIP("corpus holds no module with more than one subsong");
    }
}

TEST_CASE("a subsong number past the end is clamped", "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(64);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    // jaytrax_changeSubsong() rejects `subsongnr > nrofsongs`, which lets a
    // number *equal* to the count through -- and subsongs[] holds exactly
    // `nrofsongs` pointers, so that one reads past the end and plays whatever
    // the allocator left there. Cog takes the fragment straight off the URL,
    // so `something.jxs#3` on a three-subsong module is that read.
    //
    // Clamping means the last subsong plays instead, which is checkable: the
    // same audio as asking for it by number.
    for (const fs::path& path : modules) {
        const std::vector<Url> songs =
            registry().expandContainer(Url::fromLocalPath(path));
        if (songs.size() < 2) {
            continue;
        }
        INFO(path.filename().string());

        const Url last  = songs.back();
        const Url wild  = Url::fromLocalPath(path).withFragment("9999");
        const Url count = Url::fromLocalPath(path).withFragment(
            std::to_string(songs.size()));  // exactly one past the end

        const Decoded expected = decode(last, kTenSeconds / 8);
        REQUIRE_FALSE(expected.samples.empty());

        for (const Url& asked : {wild, count}) {
            const Decoded got = decode(asked, kTenSeconds / 8);
            REQUIRE(got.samples.size() == expected.samples.size());
            CHECK(std::equal(got.samples.begin(), got.samples.end(),
                             expected.samples.begin()));
        }
        return;
    }
    SKIP("corpus holds no module with more than one subsong");
}

TEST_CASE("seeking is exact, not approximate", "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(16);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    constexpr std::size_t  kFrames = 44100 * 8;
    // Deliberately not a multiple of the 2,048-frame render size: the seek
    // leaves the decoder mid-chunk relative to where a straight decode would
    // be, so this also says the replayer's output does not depend on how the
    // request is divided up.
    constexpr std::int64_t kTarget = (44100 * 4) + 777;

    bool tested = false;
    for (const fs::path& path : modules) {
        INFO(path.filename().string());
        const Url     url      = Url::fromLocalPath(path);
        const Decoded straight = decode(url, kFrames * 2);
        if (straight.samples.size() < kFrames * 2 || peak(straight.samples) == 0) {
            continue;  // Too short to seek four seconds into.
        }

        PluginRegistry::OpenResult opened = registry().open(url);
        REQUIRE(opened);
        REQUIRE(opened.decoder->seek(kTarget) == kTarget);

        const std::vector<std::int16_t> after = drain(*opened.decoder, 44100 * 2);
        const std::size_t               common =
            std::min(after.size(),
                     straight.samples.size() - (static_cast<std::size_t>(kTarget) * 2));
        REQUIRE(common > 44100);

        // Bit-identical, and that word is the point. Rendering the gap puts the
        // replayer in the state it would have been in; the null-buffer fast
        // path Cog seeks with does not, and every voice arrives at the seek
        // point with a stale sample position and an empty echo line.
        CHECK(std::equal(
            after.begin(), after.begin() + static_cast<std::ptrdiff_t>(common),
            straight.samples.begin() + (static_cast<std::ptrdiff_t>(kTarget) * 2)));
        tested = true;
        break;
    }
    if (!tested) {
        SKIP("corpus holds no module long enough to seek four seconds into");
    }
}

TEST_CASE("seeking backwards replays from the top", "[syntrax][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_SYNTRAX_CORPUS=<path> to run this");
    }
    const auto modules = findModules(1);
    if (modules.empty()) {
        SKIP("corpus holds no .jxs files");
    }

    PluginRegistry::OpenResult opened =
        registry().open(Url::fromLocalPath(modules.front()));
    REQUIRE(opened);

    const std::vector<std::int16_t> first = drain(*opened.decoder, 44100 * 2);
    REQUIRE_FALSE(first.empty());

    // Backwards is the case a forward-only sequencer cannot serve without
    // starting over, and starting over has to reach the same state it reached
    // the first time -- so the same frames come out.
    REQUIRE(opened.decoder->seek(0) == 0);
    const std::vector<std::int16_t> again = drain(*opened.decoder, 44100 * 2);
    REQUIRE(again.size() >= first.size());
    CHECK(std::equal(first.begin(), first.end(), again.begin()));
}
