// MP3 seek accuracy, measured rather than assumed.
//
// The chapter-reader investigation (docs/MINIMP3-CHAPTER-PROBE.md) measured
// minimp3-ex's seeking with a scratch probe and found zero positional error
// on every target; this is that measurement made permanent, through the
// shipped decoder path -- registry open, IDecoder::seek, readAudio -- so the
// ISource I/O and the gapless overrides are exercised, not just the library.
//
// Method: decode the whole file sequentially as a reference; then for each
// target, seek and read a window, and cross-correlate against the reference
// over +-8 frames to find where the decoder actually landed. The positional
// error is that shift, and it must be zero: the frame index stores exact
// sample counts and seek is arithmetic on them. The content transient -- the
// bit reservoir rebuilding from less history -- is bounded separately, with
// the tolerance the existing seek test carries for files from the wild; on
// these clean LAME encodes the aligned residual is zero.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include "../TestShell.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

namespace fs = std::filesystem;

constexpr int kRate    = 44100;
constexpr int kSeconds = 60;

fs::path fixtureDir() {
    static const fs::path dir = [] {
        const fs::path path = fs::temp_directory_path() / "xpcog-mp3-seek-tests";
        std::error_code ignored;
        fs::remove_all(path, ignored);
        fs::create_directories(path, ignored);
        return path;
    }();
    return dir;
}

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// Three summed sines: broadband enough that a correlation window has one
/// unambiguous match, deterministic so the reference is exact.
bool encode(const fs::path& mp3, const std::string& args, bool stereo) {
    if (!xpcog::test::haveTool("ffmpeg")) {
        return false;
    }
    const std::string expr =
        stereo ? "0.2*sin(2*PI*220*t)+0.15*sin(2*PI*587*t)|0.1*sin(2*PI*1234*t)+0.15*sin(2*PI*330*t)"
               : "0.2*sin(2*PI*220*t)+0.15*sin(2*PI*587*t)+0.1*sin(2*PI*1234*t)";
    std::error_code ignored;
    fs::remove(mp3, ignored);
    const std::string command = "ffmpeg -loglevel error -y -f lavfi -i \"aevalsrc=" + expr +
                                ":s=" + std::to_string(kRate) + ":d=" + std::to_string(kSeconds) +
                                "\" -c:a libmp3lame " + args + " \"" + mp3.string() + "\"" +
                                xpcog::test::kSilenceStderr;
    return std::system(command.c_str()) == 0 && fs::exists(mp3);
}

/// The whole file, decoded sequentially from sample zero: the reference every
/// seek is measured against. Interleaved floats, as the decoder hands them out.
std::vector<float> decodeAll(IDecoder& decoder, int& channels) {
    std::vector<float> out;
    AudioChunk         chunk;
    while (decoder.readAudio(chunk)) {
        const auto* data = reinterpret_cast<const float*>(chunk.bytes().data());
        const std::size_t got = chunk.frameCount() * chunk.format().channels;
        out.insert(out.end(), data, data + got);
        channels = static_cast<int>(chunk.format().channels);
    }
    return out;
}

/// N frames from the current position, interleaved.
std::vector<float> readFrames(IDecoder& decoder, std::size_t frames, int channels) {
    std::vector<float> out;
    out.reserve(frames * static_cast<std::size_t>(channels));
    AudioChunk chunk;
    while (out.size() < frames * static_cast<std::size_t>(channels) &&
           decoder.readAudio(chunk)) {
        const auto* data = reinterpret_cast<const float*>(chunk.bytes().data());
        const std::size_t got = chunk.frameCount() * chunk.format().channels;
        out.insert(out.end(), data, data + got);
    }
    return out;
}

constexpr std::size_t kWindow     = 8192;  // frames read after each seek
constexpr std::size_t kCorr       = 4096;  // frames used for the shift search
constexpr int         kShiftRange = 8;     // +- frames searched

void measure(const fs::path& mp3) {
    auto reference = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(reference);
    int                channels = 0;
    const std::vector<float> ref = decodeAll(*reference.decoder, channels);
    REQUIRE(channels > 0);
    const std::size_t totalFrames = ref.size() / static_cast<std::size_t>(channels);
    REQUIRE(totalFrames > kWindow);

    auto opened = registry().open(Url::fromLocalPath(mp3));
    REQUIRE(opened);
    IDecoder& decoder = *opened.decoder;

    // Frame boundaries and their neighbours, a chapter-style 45 s boundary,
    // arbitrary mid-file points, and near the end.
    std::vector<std::size_t> targets = {0, 1, 1151, 1152, 1153, 44100,
                                        totalFrames * 2 / 3, 1234567,
                                        totalFrames - kWindow, 45ULL * kRate};
    for (const std::size_t target : targets) {
        if (target + kWindow > totalFrames) {
            continue;
        }
        INFO("target frame " << target);
        REQUIRE(decoder.seek(static_cast<std::int64_t>(target)) ==
                static_cast<std::int64_t>(target));
        const std::vector<float> win = readFrames(decoder, kWindow, channels);
        REQUIRE(win.size() >= kCorr * static_cast<std::size_t>(channels));

        // Where did it actually land? The shift whose reference window
        // matches best.
        int    bestShift = 0;
        double bestSse   = -1.0;
        for (int shift = -kShiftRange; shift <= kShiftRange; ++shift) {
            const std::int64_t base = static_cast<std::int64_t>(target) + shift;
            if (base < 0 ||
                static_cast<std::size_t>(base + kCorr) > totalFrames) {
                continue;
            }
            double sse = 0.0;
            for (std::size_t i = 0; i < kCorr * static_cast<std::size_t>(channels); ++i) {
                const double d =
                    static_cast<double>(win[i]) -
                    static_cast<double>(
                        ref[static_cast<std::size_t>(base) * static_cast<std::size_t>(channels) + i]);
                sse += d * d;
            }
            if (bestSse < 0.0 || sse < bestSse) {
                bestSse   = sse;
                bestShift = shift;
            }
        }
        CHECK(bestShift == 0);

        // The transient a listener hears at the boundary: worst difference
        // over the first two frames, unaligned; and the residual once
        // aligned, which is zero on a clean LAME encode.
        float worstUnaligned = 0.0f;
        float worstAligned   = 0.0f;
        const std::size_t twoFrames = 2304 * static_cast<std::size_t>(channels);
        const std::size_t base =
            (static_cast<std::int64_t>(target) + bestShift) * static_cast<std::size_t>(channels);
        for (std::size_t i = 0; i < twoFrames; ++i) {
            worstUnaligned =
                std::max(worstUnaligned,
                         std::fabs(win[i] - ref[target * static_cast<std::size_t>(channels) + i]));
            worstAligned = std::max(worstAligned, std::fabs(win[i] - ref[base + i]));
        }
        CHECK(worstUnaligned <= 0.1f);
        CHECK(worstAligned < 1e-3f);
    }
}

}  // namespace

TEST_CASE("MP3 seeking lands on the exact sample: CBR with Xing", "[mp3][seek][accuracy]") {
    const fs::path mp3 = fixtureDir() / "cbr-xing.mp3";
    if (!encode(mp3, "-b:a 128k", false)) {
        SKIP("ffmpeg with libmp3lame is not installed");
    }
    measure(mp3);
}

TEST_CASE("MP3 seeking lands on the exact sample: CBR without Xing", "[mp3][seek][accuracy]") {
    const fs::path mp3 = fixtureDir() / "cbr-noxing.mp3";
    if (!encode(mp3, "-b:a 128k -write_xing 0", false)) {
        SKIP("ffmpeg with libmp3lame is not installed");
    }
    measure(mp3);
}

TEST_CASE("MP3 seeking lands on the exact sample: VBR with Xing", "[mp3][seek][accuracy]") {
    const fs::path mp3 = fixtureDir() / "vbr-xing.mp3";
    if (!encode(mp3, "-qscale:a 2", false)) {
        SKIP("ffmpeg with libmp3lame is not installed");
    }
    measure(mp3);
}

TEST_CASE("MP3 seeking lands on the exact sample: VBR without Xing", "[mp3][seek][accuracy]") {
    const fs::path mp3 = fixtureDir() / "vbr-noxing.mp3";
    if (!encode(mp3, "-qscale:a 2 -write_xing 0", false)) {
        SKIP("ffmpeg with libmp3lame is not installed");
    }
    measure(mp3);
}

TEST_CASE("MP3 seeking lands on the exact sample: stereo VBR", "[mp3][seek][accuracy]") {
    const fs::path mp3 = fixtureDir() / "stereo-vbr.mp3";
    if (!encode(mp3, "-qscale:a 2", true)) {
        SKIP("ffmpeg with libmp3lame is not installed");
    }
    measure(mp3);
}
