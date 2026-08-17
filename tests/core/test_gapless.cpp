// Gapless playback is the reason people use Cog, and it is the easiest thing to
// break silently: a handoff that drops or repeats even a few frames sounds like a
// tick, not like a crash. These tests run the real engine against an offline
// output, so they are deterministic and need no audio device.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/SampleConvert.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kChannels   = 2;

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-gapless-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

/// Writes a 16-bit stereo WAV of a continuous sine, starting at `startFrame` so
/// two consecutive files join into one unbroken tone. That continuity is what
/// makes a seam defect measurable: any drop or repeat shows up as a step.
std::filesystem::path writeWav(const std::string& name, int startFrame, int frames,
                               double freq = 440.0) {
    const auto path = fixtureDir() / name;

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames) * kChannels);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(startFrame + i) / kSampleRate;
        const auto   v = static_cast<std::int16_t>(20000.0 * std::sin(2.0 * M_PI * freq * t));
        samples.push_back(v);
        samples.push_back(v);
    }

    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t byteRate = static_cast<std::uint32_t>(kSampleRate) * kChannels * 2;

    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);

    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);                                              // PCM
    u16(kChannels);
    u32(static_cast<std::uint32_t>(kSampleRate));
    u32(byteRate);
    u16(kChannels * 2);                                  // block align
    u16(16);                                             // bits
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);

    return path;
}

/// Converts a WAV to FLAC using the `flac` CLI. Returns nullopt when the tool is
/// unavailable, so the suite degrades to skipping rather than failing on a
/// machine without it.
std::optional<std::filesystem::path> makeFlac(const std::string& name, int startFrame,
                                              int frames, double freq = 440.0) {
    const auto wav  = writeWav(name + ".wav", startFrame, frames, freq);
    const auto flac = fixtureDir() / (name + ".flac");

    const std::string command = "flac -s -f --totally-silent -o \"" + flac.string() +
                                "\" \"" + wav.string() + "\" 2>/dev/null";
    if (std::system(command.c_str()) != 0) {
        return std::nullopt;
    }
    if (!std::filesystem::exists(flac)) {
        return std::nullopt;
    }
    return flac;
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

/// Decodes a file straight through the decoder, bypassing the engine. This is the
/// reference the engine's output is compared against.
std::vector<float> decodeDirect(const std::filesystem::path& path) {
    auto opened = registry().open(Url::fromLocalPath(path));
    REQUIRE(opened);

    std::vector<float> out;
    AudioChunk         chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t samples = float32SampleCount(chunk);
        const std::size_t offset  = out.size();
        out.resize(offset + samples);
        convertToFloat32(chunk, std::span<float>{out}.subspan(offset, samples));
    }
    return out;
}

/// Plays `urls` back to back through the engine and returns everything captured.
struct PlaylistDelegate final : AudioEngine::Delegate {
    std::vector<Url> queue;
    std::size_t      next = 0;
    std::vector<Url> began;

    std::optional<Url> nextTrack() override {
        if (next >= queue.size()) {
            return std::nullopt;
        }
        return queue[next++];
    }
    void trackBegan(const Url& url) override { began.push_back(url); }
};

std::vector<float> playThrough(const std::vector<std::filesystem::path>& paths,
                               std::vector<Url>*                        began = nullptr) {
    RingBuffer ring(static_cast<std::size_t>(kSampleRate * 0.5) * kChannels);
    auto       output = makeOfflineOutput(ring);

    auto        store = makeMemorySettingsStore();
    Settings    settings(*store);
    AudioEngine engine(registry(), *output, ring, settings);
    PlaylistDelegate delegate;
    for (std::size_t i = 1; i < paths.size(); ++i) {
        delegate.queue.push_back(Url::fromLocalPath(paths[i]));
    }
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(paths.front())));
    engine.waitUntilFinished();
    engine.stop();

    if (began != nullptr) {
        *began = delegate.began;
    }
    return capturedAudio(*output);
}

}  // namespace

TEST_CASE("gapless handoff is sample-exact across a track boundary", "[gapless]") {
    // Two halves of one continuous 440 Hz tone, 1 second each.
    const auto first  = makeFlac("seam_a", 0, 44100);
    const auto second = makeFlac("seam_b", 44100, 44100);
    if (!first || !second) {
        SKIP("the `flac` command-line tool is not available");
    }

    const std::vector<float> reference = [&] {
        std::vector<float> a = decodeDirect(*first);
        const std::vector<float> b = decodeDirect(*second);
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }();

    const std::vector<float> played = playThrough({*first, *second});

    // Nothing may be dropped or duplicated at the seam.
    REQUIRE(played.size() == reference.size());
    for (std::size_t i = 0; i < played.size(); ++i) {
        if (played[i] != reference[i]) {
            FAIL("sample " << i << " differs: " << played[i] << " vs " << reference[i]);
        }
    }
}

TEST_CASE("gapless seam has no audible discontinuity", "[gapless]") {
    const auto first  = makeFlac("cont_a", 0, 44100);
    const auto second = makeFlac("cont_b", 44100, 44100);
    if (!first || !second) {
        SKIP("the `flac` command-line tool is not available");
    }

    const std::vector<float> played = playThrough({*first, *second});
    REQUIRE(played.size() == 44100 * 2 * kChannels);

    // Sample-exactness alone would not catch a seam that is continuous but
    // misaligned, so check the waveform directly: across a join in a 440 Hz tone
    // no adjacent pair may jump more than a single-sample step ever does.
    const std::size_t seam = 44100 * kChannels;
    double            maxStepNearSeam = 0.0;
    for (std::size_t i = seam - 20; i < seam + 20; i += kChannels) {
        maxStepNearSeam =
            std::max(maxStepNearSeam,
                     std::abs(static_cast<double>(played[i]) - played[i - kChannels]));
    }

    // One sample of 440 Hz at 44.1 kHz advances at most ~0.0627 of full scale;
    // the tone is at 0.61 amplitude, so ~0.039. Allow generous headroom -- a real
    // dropout or repeat produces a step an order of magnitude larger.
    CHECK(maxStepNearSeam < 0.1);
}

TEST_CASE("track changes are announced in order", "[gapless]") {
    const auto first  = makeFlac("notify_a", 0, 22050);
    const auto second = makeFlac("notify_b", 22050, 22050);
    if (!first || !second) {
        SKIP("the `flac` command-line tool is not available");
    }

    std::vector<Url> began;
    playThrough({*first, *second}, &began);

    REQUIRE(began.size() == 2);
    CHECK(began[0] == Url::fromLocalPath(*first));
    CHECK(began[1] == Url::fromLocalPath(*second));
}

TEST_CASE("a single track plays through unchanged", "[gapless]") {
    const auto only = makeFlac("single", 0, 30000);
    if (!only) {
        SKIP("the `flac` command-line tool is not available");
    }

    const std::vector<float> reference = decodeDirect(*only);
    const std::vector<float> played    = playThrough({*only});

    REQUIRE(played.size() == reference.size());
    CHECK(played == reference);
}

TEST_CASE("a track at a different sample rate still joins gaplessly", "[gapless]") {
    // The case that used to end playback. The device stays at the first track's
    // rate and later tracks are resampled into it, so a 44.1 kHz album followed
    // by a 48 kHz track plays straight through.
    const auto first = makeFlac("rate_a", 0, 22050);
    if (!first) {
        SKIP("the `flac` command-line tool is not available");
    }

    // Same tone, written at 48 kHz.
    const auto wav48 = fixtureDir() / "rate_b.wav";
    {
        std::vector<std::int16_t> samples;
        for (int i = 0; i < 48000; ++i) {
            const auto v = static_cast<std::int16_t>(
                20000.0 * std::sin(2.0 * M_PI * 440.0 * (i / 48000.0)));
            samples.push_back(v);
            samples.push_back(v);
        }
        const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
        std::FILE* f         = std::fopen(wav48.string().c_str(), "wb");
        REQUIRE(f != nullptr);
        const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(1); u16(2);
        u32(48000); u32(48000 * 4); u16(4); u16(16);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        std::fwrite(samples.data(), 1, dataBytes, f);
        std::fclose(f);
    }
    const auto flac48 = fixtureDir() / "rate_b.flac";
    const std::string cmd = "flac -s -f --totally-silent -o \"" + flac48.string() +
                            "\" \"" + wav48.string() + "\" 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
        SKIP("could not encode the 48 kHz fixture");
    }

    std::vector<Url> began;
    const std::vector<float> played = playThrough({*first, flac48}, &began);

    // Both tracks were actually played -- previously the second was dropped.
    REQUIRE(began.size() == 2);

    // 0.5 s at 44.1 kHz plus 1 s resampled from 48 kHz to 44.1 kHz.
    const double seconds = static_cast<double>(played.size() / kChannels) / kSampleRate;
    CHECK(seconds == Catch::Approx(1.5).margin(0.05));
}

TEST_CASE("three tracks join without accumulating drift", "[gapless]") {
    // Two joins rather than one: an off-by-one at each seam would cancel in a
    // length check over a single boundary but accumulates over two.
    const auto a = makeFlac("tri_a", 0, 10000);
    const auto b = makeFlac("tri_b", 10000, 10000);
    const auto c = makeFlac("tri_c", 20000, 10000);
    if (!a || !b || !c) {
        SKIP("the `flac` command-line tool is not available");
    }

    const std::vector<float> played = playThrough({*a, *b, *c});
    CHECK(played.size() == 30000 * kChannels);
}
