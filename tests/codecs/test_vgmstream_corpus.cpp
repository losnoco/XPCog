// Decoding real game rips, from a corpus that is not in this repository.
//
// Every fixture these formats need is a rip of copyrighted game audio, so none
// of it can be checked in -- which left the vgmstream decoder with no automated
// decode coverage at all, and its worst bug (every rip decoding to zero frames,
// silently) was exactly the kind a decode test catches on the first run.
//
// The answer is to point the suite at a corpus that already exists on the
// machine running it. Configure with
//
//     -DXPCOG_VGM_CORPUS=D:/vgm
//
// and these cases walk it; without it they skip, so CI and every other clone are
// unaffected and nothing copyrighted is ever committed or downloaded.
//
// Bounded on purpose. A few seconds of audio from a couple of files per format
// is enough to prove the decoder produces real samples -- one ADX in the corpus
// is seven minutes long, and decoding the lot would turn a test run into a
// coffee break.

#include "xpcog/core/LoopPolicy.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <span>
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

#ifdef XPCOG_VGM_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_VGM_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

/// The formats this decoder was added for. Not every corpus holds all of them,
/// so a missing one is skipped rather than failed -- what is being tested is the
/// decoder, not the completeness of somebody's collection.
constexpr const char* kFormats[] = {"brstm", "bcstm", "bfstm", "bwav", "bfwav",
                                    "hps",   "ast",   "adx",   "aax",  "dsp",
                                    "ssm",   "afc",   "bns"};

constexpr int         kFilesPerFormat = 2;
constexpr std::size_t kMaxFrames      = 96000;  ///< ~2 s at 48 kHz

/// Up to `kFilesPerFormat` files of each wanted extension, found in one walk.
/// One traversal rather than one per format: the corpus this was written
/// against holds 5,318 files and thirteen walks of it is thirteen times the I/O
/// for the same answer.
[[nodiscard]] std::map<std::string, std::vector<fs::path>> findSamples() {
    std::map<std::string, std::vector<fs::path>> found;
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
        if (!entry.is_regular_file(error)) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        if (extension.size() < 2) {
            continue;
        }
        extension.erase(0, 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (std::find(std::begin(kFormats), std::end(kFormats), extension) ==
            std::end(kFormats)) {
            continue;
        }
        auto& list = found[extension];
        if (list.size() < static_cast<std::size_t>(kFilesPerFormat)) {
            list.push_back(entry.path());
        }
    }
    return found;
}

struct DecodeResult {
    std::size_t frames  = 0;
    float       peak    = 0.0F;
    int         channels = 0;
    double      rate     = 0.0;
};

/// Decodes the opening seconds. Returns zeroes if the file will not open, which
/// the caller reports as a failure -- an unopenable file in the corpus is the
/// regression this exists to catch.
[[nodiscard]] DecodeResult decodeOpening(const fs::path& path) {
    auto opened = registry().open(Url::fromLocalPath(path));
    if (!opened) {
        return {};
    }

    DecodeResult result;
    const TrackProperties props = opened.decoder->properties();
    result.channels             = static_cast<int>(props.format.channels);
    result.rate                 = props.format.sampleRate;

    std::vector<float> samples;
    AudioChunk         chunk;
    while (result.frames < kMaxFrames && opened.decoder->readAudio(chunk)) {
        const std::size_t count = float32SampleCount(chunk);
        samples.resize(count);
        convertToFloat32(chunk, std::span<float>{samples});
        for (const float value : samples) {
            result.peak = std::max(result.peak, std::abs(value));
        }
        result.frames += chunk.frameCount();
    }
    return result;
}

}  // namespace

TEST_CASE("game rips decode to real audio", "[vgmstream][corpus]") {
    if (!kHaveCorpus) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }
    if (!fs::exists(corpusRoot())) {
        SKIP("XPCOG_VGM_CORPUS points at nothing");
    }

    const auto samples = findSamples();
    // A corpus that matched nothing means the path is wrong or holds no rips,
    // and silently passing would make this test decorative.
    REQUIRE_FALSE(samples.empty());

    for (const auto& [extension, paths] : samples) {
        for (const fs::path& path : paths) {
            INFO(extension << ": " << path.filename().string());

            const DecodeResult result = decodeOpening(path);
            // Opened at all.
            CHECK(result.channels > 0);
            CHECK(result.rate > 0.0);
            // Produced samples. This is the assertion that would have caught
            // libvgmstream_fill() returning zero forever.
            CHECK(result.frames > 0);
            // And they are audio rather than a buffer of silence, which is what
            // a decoder wired to the wrong sample format tends to hand back.
            CHECK(result.peak > 0.0F);
        }
    }
}

TEST_CASE("a rip's reported duration matches what it decodes",
          "[vgmstream][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }

    const auto samples = findSamples();
    REQUIRE_FALSE(samples.empty());

    // One short file, decoded whole. The frame count the decoder reports up
    // front drives the seek bar and the playlist's total, so a decoder that
    // produces good audio while lying about its length still shows up as a
    // track that ends early or hangs at the end.
    const fs::path* shortest = nullptr;
    std::uintmax_t  smallest = 0;
    for (const auto& [extension, paths] : samples) {
        for (const fs::path& path : paths) {
            std::error_code      error;
            const std::uintmax_t size = fs::file_size(path, error);
            if (error || size == 0) {
                continue;
            }
            if (shortest == nullptr || size < smallest) {
                shortest = &path;
                smallest = size;
            }
        }
    }
    REQUIRE(shortest != nullptr);
    INFO(shortest->filename().string());

    auto opened = registry().open(Url::fromLocalPath(*shortest));
    REQUIRE(opened);
    const std::int64_t claimed = opened.decoder->properties().totalFrames;
    REQUIRE(claimed > 0);

    std::int64_t decoded = 0;
    AudioChunk   chunk;
    while (opened.decoder->readAudio(chunk)) {
        decoded += static_cast<std::int64_t>(chunk.frameCount());
    }

    // Exact, because vgmstream reports the play length it will actually render
    // under the config it was given. A mismatch means the loop and fade settings
    // the decoder asks for are not the ones it decodes under.
    CHECK(decoded == claimed);
}

TEST_CASE("a looping stream plays for ever under repeat-one",
          "[vgmstream][corpus][loop]") {
    if (!kHaveCorpus) {
        SKIP("no corpus: configure with -DXPCOG_VGM_CORPUS=<path> to run this");
    }

    // Game music loops because the game did, and one loop then stop is what a
    // playlist wants. Repeat-one is the listener saying otherwise, and this is
    // the only place in the suite where that can be checked against a stream
    // that actually declares a loop point -- synthetic fixtures have none.
    //
    // The test finds its own subject: a file whose endless decode outruns its
    // finite one is, by definition, a looping one. Nothing in TrackProperties
    // reports a loop flag, so asking the decoder twice is the question.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setRepeatMode(static_cast<int>(RepeatMode::One));

    PluginRegistry registry;
    registerAllCodecs(registry);
    registry.setSettings(&settings);

    constexpr std::size_t kProbeFrames = 48000 * 30;  ///< 30 s at 48 kHz
    constexpr int         kMaxFiles    = 12;

    const auto readUpTo = [](IDecoder& decoder, std::size_t limit) {
        std::size_t frames = 0;
        AudioChunk  chunk;
        while (frames < limit && decoder.readAudio(chunk)) {
            frames += chunk.frameCount();
        }
        return frames;
    };

    int examined = 0;
    for (const auto& [format, paths] : findSamples()) {
        for (const fs::path& path : paths) {
            if (examined >= kMaxFiles) {
                break;
            }
            const Url url = Url::fromLocalPath(path);

            // Never, so this half is unaffected by the repeat setting above --
            // which is also the override a converter would pass.
            auto finite = registry.open(url, SkipCue::No, LoopPolicy::Never);
            if (!finite) {
                continue;
            }
            const std::size_t finiteFrames = readUpTo(*finite.decoder, kProbeFrames);
            if (finiteFrames == 0 || finiteFrames >= kProbeFrames) {
                continue;  // empty, or too long to establish an ending cheaply
            }
            ++examined;

            auto endless = registry.open(url);
            REQUIRE(endless);
            const std::size_t endlessFrames =
                readUpTo(*endless.decoder, finiteFrames + 48000);

            if (endlessFrames <= finiteFrames) {
                continue;  // this one simply has no loop point
            }

            INFO("looping stream: " << path.filename().string());
            // It outran its own ending, which only a loop can do.
            CHECK(endlessFrames > finiteFrames);
            // And the reported length is unchanged: repeat-one makes a track
            // play on, it does not make it claim to be longer. Cog behaves the
            // same, and a length that grew would move the seek bar under the
            // listener.
            CHECK(endless.decoder->properties().totalFrames ==
                  finite.decoder->properties().totalFrames);
            return;
        }
    }

    SKIP("no looping stream found in the corpus");
}
