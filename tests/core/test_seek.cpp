// Seeking through the engine.
//
// The interesting part is not that the decoder can seek -- the codec tests cover
// that -- but that the audio already handed to the device is discarded. Without
// that, a seek is followed by up to a ring's worth of audio from the old
// position, which is exactly the artefact a user notices.

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;

TEST_CASE("a flush request drops what is buffered, once", "[seek]") {
    RingBuffer ring{1024};

    const std::vector<float> stale(200, 1.0F);
    REQUIRE(ring.write(stale.data(), stale.size()) == 200);
    REQUIRE(ring.availableToRead() == 200);

    ring.requestFlush();
    REQUIRE(ring.flushPending());

    // The consumer honours it, and reports nothing this call: the stale samples
    // are gone rather than delivered.
    std::vector<float> out(200, -1.0F);
    REQUIRE(ring.read(out.data(), out.size()) == 0);
    REQUIRE_FALSE(ring.flushPending());
    REQUIRE(ring.availableToRead() == 0);

    // Post-flush audio is delivered normally; the flush does not latch.
    const std::vector<float> fresh(50, 0.5F);
    REQUIRE(ring.write(fresh.data(), fresh.size()) == 50);
    REQUIRE(ring.read(out.data(), out.size()) == 50);
    REQUIRE(out[0] == 0.5F);
}

TEST_CASE("clear does not strand a pending flush", "[seek]") {
    RingBuffer ring{1024};

    const std::vector<float> samples(10, 1.0F);
    REQUIRE(ring.write(samples.data(), samples.size()) == 10);
    ring.requestFlush();
    REQUIRE(ring.flushPending());

    // A stop clears the ring outright. Leaving the request outstanding would
    // make the next play() sit forever waiting for an acknowledgement from a
    // consumer that has nothing left to acknowledge.
    ring.clear();
    REQUIRE_FALSE(ring.flushPending());
}

TEST_CASE("a flush is honoured by the reader that runs concurrently", "[seek]") {
    RingBuffer ring{4096};

    std::atomic<bool>        stop{false};
    std::atomic<std::size_t> delivered{0};

    // Consumer, standing in for the audio callback.
    std::thread consumer([&] {
        std::vector<float> out(64);
        while (!stop.load(std::memory_order_relaxed)) {
            delivered.fetch_add(ring.read(out.data(), out.size()),
                                std::memory_order_relaxed);
        }
    });

    const std::vector<float> block(512, 1.0F);
    for (int i = 0; i < 8; ++i) {
        std::size_t written = 0;
        while (written < block.size()) {
            written += ring.write(block.data() + written, block.size() - written);
        }
    }

    ring.requestFlush();
    // The producer must wait rather than write: this is the rendezvous the seek
    // path relies on.
    while (ring.flushPending()) {
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    REQUIRE_FALSE(ring.flushPending());
}

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kChannels   = 2;

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-seek-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

/// A continuous sine as 16-bit stereo FLAC. nullopt when `flac` is missing, so
/// the suite skips rather than fails on a machine without it.
std::optional<std::filesystem::path> makeFlac(const std::string& name, int frames) {
    const auto wav  = fixtureDir() / (name + ".wav");
    const auto flac = fixtureDir() / (name + ".flac");

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames) * kChannels);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const auto   v =
            static_cast<std::int16_t>(20000.0 * std::sin(xpcog::test::kTwoPi * 440.0 * t));
        samples.push_back(v);
        samples.push_back(v);
    }

    const auto dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    std::FILE* f = std::fopen(wav.string().c_str(), "wb");
    if (f == nullptr) {
        return std::nullopt;
    }
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(kChannels);
    u32(static_cast<std::uint32_t>(kSampleRate));
    u32(static_cast<std::uint32_t>(kSampleRate) * kChannels * 2);
    u16(kChannels * 2);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);

    const std::string command = "flac -s -f --totally-silent -o \"" + flac.string() +
                                "\" \"" + wav.string() + "\"" +
                                xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0) {
        return std::nullopt;
    }
    return flac;
}

const PluginRegistry& registry() {
    static const PluginRegistry& instance = *[] {
        auto* built = new PluginRegistry;
        registerAllCodecs(*built);
        return built;
    }();
    return instance;
}

}  // namespace

TEST_CASE("seeking skips audio and lands cleanly", "[seek]") {
    constexpr int kFrames = static_cast<int>(kSampleRate) * 4;  // four seconds

    const auto file = makeFlac("tone", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }
    const Url url = Url::fromLocalPath(*file);

    const auto playAndMaybeSeek = [&url](double seekTo) {
        RingBuffer ring{static_cast<std::size_t>(kSampleRate * 0.25) * kChannels};
        auto       output = makeOfflineOutput(ring);

        auto        store = makeMemorySettingsStore();
        Settings    settings{*store};
        AudioEngine engine{registry(), *output, ring, settings};

        REQUIRE(engine.play(url));
        if (seekTo > 0.0) {
            REQUIRE(engine.seek(seekTo));
        }
        engine.waitUntilFinished();
        engine.stop();
        return capturedAudio(*output).size();
    };

    const std::size_t whole = playAndMaybeSeek(0.0);
    REQUIRE(whole > 0);

    // Seeking three seconds into a four-second file must leave materially less
    // to play. The bound is loose because how much was already in flight when
    // the seek landed depends on scheduling -- but "skipped most of it" does
    // not.
    const std::size_t afterSeek = playAndMaybeSeek(3.0);
    REQUIRE(afterSeek < whole / 2);
}

TEST_CASE("the position reports from the new place after a seek", "[seek]") {
    // What the seek bar reads back. The base for this must not include the
    // frames the flush discards: they are never delivered, so framesPlayed()
    // never accounts for them, and a base that counts them sits up to a whole
    // ring ahead of reality. Against a real device that means the clock keeps
    // counting from the *old* position for as long as the ring is deep --
    // about three seconds, in the application.
    //
    // The offline output drains as fast as it can rather than at 1x, so it
    // cannot reproduce that stall: the wall-clock symptom needs a real device.
    // This checks the part that is observable here -- that the position
    // arrives at the sought location rather than somewhere else.
    constexpr int kFrames = static_cast<int>(kSampleRate) * 8;

    const auto file = makeFlac("position", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kSampleRate * 0.5) * kChannels};

    // Paced, because this test has to seek while the track is still playing.
    // Unlimited, the eight seconds are consumed in about the time it takes to
    // decode them -- tens of milliseconds -- so the first position poll already
    // read 8.0, the track had ended, and seek() correctly refused. At 8x, the
    // seek target arrives in well under a second and the poll loops below have
    // room to observe it.
    auto output = makeOfflineOutput(ring, 8.0);

    auto        store = makeMemorySettingsStore();
    Settings    settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    for (int i = 0; i < 200 && engine.trackPositionSeconds() < 0.5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(engine.trackPositionSeconds() >= 0.5);

    REQUIRE(engine.seek(6.0));

    bool reachedTarget = false;
    for (int i = 0; i < 400; ++i) {
        if (engine.trackPositionSeconds() >= 6.0) {
            reachedTarget = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.stop();

    REQUIRE(reachedTarget);
}

TEST_CASE("seeking past the end does not hang", "[seek]") {
    constexpr int kFrames = static_cast<int>(kSampleRate);

    const auto file = makeFlac("short", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kSampleRate * 0.25) * kChannels};
    auto       output = makeOfflineOutput(ring);

    auto        store = makeMemorySettingsStore();
    Settings    settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(engine.seek(60.0));  // well past the end
    engine.waitUntilFinished();  // must terminate rather than wait forever
    engine.stop();
}
