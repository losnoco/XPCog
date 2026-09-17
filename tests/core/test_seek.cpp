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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
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
///
/// `amplitude` is what the two-track test below tells its tracks apart by: the
/// engine resamples nothing here and applies no gain, so a window's peak level
/// says which file is being heard.
std::optional<std::filesystem::path> makeFlac(const std::string& name, int frames,
                                              double amplitude = 20000.0) {
    const auto wav  = fixtureDir() / (name + ".wav");
    const auto flac = fixtureDir() / (name + ".flac");

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(frames) * kChannels);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const auto   v =
            static_cast<std::int16_t>(amplitude * std::sin(xpcog::test::kTwoPi * 440.0 * t));
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
        // Paced, because this test acts on playback while it is still under way.
        // Unpaced, a four-second file drains in the time it takes to decode it,
        // and on a fast enough machine the whole thing reaches the capture
        // before the seek is serviced -- so the seek skips nothing and the
        // measurement is of scheduling rather than of seeking. That is not
        // hypothetical: it passed here fifteen times out of fifteen and failed
        // on a macOS runner. See makeOfflineOutput's own note.
        auto output = makeOfflineOutput(ring, 8.0);

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
    // to play. The bound stays loose because how much was already in flight when
    // the seek landed still depends on scheduling -- pacing bounds that window
    // rather than removing it -- but "skipped most of it" does not.
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

namespace {

/// Frames whose local peak level sits in [low, high].
///
/// The two tracks in the test below differ only in level, and one sample cannot
/// say which is playing -- both cross zero -- so the measurement is per window.
/// 512 frames is five cycles of 440 Hz, short enough that the few windows
/// straddling a join are a rounding error against seconds of audio.
std::size_t framesAtLevel(const std::vector<float>& samples, float low, float high) {
    constexpr std::size_t kWindow = 512;

    const std::size_t total  = samples.size() / kChannels;
    std::size_t       frames = 0;
    for (std::size_t start = 0; start + kWindow <= total; start += kWindow) {
        float peak = 0.0F;
        for (std::size_t i = start * kChannels; i < (start + kWindow) * kChannels; ++i) {
            peak = std::max(peak, std::abs(samples[i]));
        }
        if (peak >= low && peak <= high) {
            frames += kWindow;
        }
    }
    return frames;
}

/// Two tracks played back to back, counting what it was asked for.
///
/// `next` and the two callbacks below all belong to the feeder thread, which is
/// the only thread that touches them; `handouts` is atomic because the test
/// thread watches it to know when the gapless handoff has happened.
struct TwoTrackDelegate final : AudioEngine::Delegate {
    std::vector<Url>   queue;
    std::size_t        next = 0;
    std::atomic<int>   handouts{0};
    std::mutex         mutex;
    std::vector<Url>   began;

    std::optional<Url> nextTrack() override {
        if (next >= queue.size()) {
            return std::nullopt;
        }
        handouts.fetch_add(1, std::memory_order_release);
        return queue[next++];
    }

    void nextTrackAbandoned(const Url& audible) override {
        // Back to the audible track rather than back by one: the engine drops
        // every handout it is holding, and it can be holding more than one.
        for (std::size_t i = 0; i < queue.size(); ++i) {
            if (queue[i].toString() == audible.toString()) {
                next = i + 1;
                return;
            }
        }
        next = 0;  // the audible track is the one play() was given
    }

    void trackBegan(const Url& url) override {
        const std::lock_guard lock(mutex);
        began.push_back(url);
    }
};

}  // namespace

TEST_CASE("a seek near the end of a track stays inside that track", "[seek][gapless]") {
    // A gapless engine opens the next track when the current one stops
    // *decoding*, which is a queue's worth of audio before it is heard -- so for
    // the last few seconds of every song the decoder that is open belongs to the
    // song after it. A seek arriving in that window was applied to that decoder:
    // dragging the slider back from near the end of a track started playing the
    // middle of the next one.
    //
    // Cog has the same window and the same answer -- re-open the track the
    // listener can hear and seek that, which its -seekToTime: calls a dirty hack
    // under endOfInputReached.
    constexpr int kFirstFrames  = static_cast<int>(kSampleRate) * 6;
    constexpr int kSecondFrames = static_cast<int>(kSampleRate) * 4;

    // Same tone, different levels, so which track a stretch of the capture came
    // from can be measured rather than inferred.
    const auto loud  = makeFlac("seam_seek_loud", kFirstFrames, 20000.0);
    const auto quiet = makeFlac("seam_seek_quiet", kSecondFrames, 6000.0);
    if (!loud || !quiet) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kSampleRate * 0.5) * kChannels};
    // Paced, because the whole test is about acting during playback: the handoff
    // has to have happened and the first track has to still be audible, and
    // unpaced there is no such moment to catch.
    auto output = makeOfflineOutput(ring, 8.0);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    // The fade a seek plays would scale the levels this test measures by.
    settings.setEnableFading(false);
    AudioEngine engine{registry(), *output, ring, settings};

    TwoTrackDelegate delegate;
    delegate.queue.push_back(Url::fromLocalPath(*quiet));
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(*loud)));

    // The window: the second track has been handed out and opened, and the first
    // is still playing out of the queue.
    bool inWindow = false;
    for (int i = 0; i < 1000; ++i) {
        if (delegate.handouts.load(std::memory_order_acquire) > 0 &&
            engine.trackPositionSeconds() >= 3.0) {
            inWindow = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(inWindow);

    REQUIRE(engine.seek(0.5));
    engine.waitUntilFinished();
    engine.stop();

    const std::vector<float> played = capturedAudio(*output);
    const std::size_t        loudFrames  = framesAtLevel(played, 0.4F, 1.0F);
    const std::size_t        quietFrames = framesAtLevel(played, 0.08F, 0.35F);

    // Most of the first track is heard twice over: up to the seek, and then from
    // half a second in to its end. Seeking the wrong decoder left three seconds
    // of it and nothing more.
    CHECK(loudFrames > static_cast<std::size_t>(kSampleRate) * 5);

    // And the second track is played in full, from its start, exactly once --
    // neither seeked into (which is the bug) nor skipped, which is what a
    // read-ahead cursor left pointing past it would do.
    CHECK(quietFrames > static_cast<std::size_t>(kSampleRate * 3.8));
    CHECK(quietFrames < static_cast<std::size_t>(kSampleRate * 4.2));

    // One track began, and then the other: the re-open is not a new track and
    // must not be announced as one -- the listener never stopped hearing the
    // first.
    const std::lock_guard lock(delegate.mutex);
    REQUIRE(delegate.began.size() == 2);
    CHECK(delegate.began.front().toString() == Url::fromLocalPath(*loud).toString());
    CHECK(delegate.began.back().toString() == Url::fromLocalPath(*quiet).toString());
}

TEST_CASE("a seek during the last track's play-out is still serviced", "[seek]") {
    // The other end of the same window. Decoding runs hundreds of times faster
    // than playback, so the feeder reaches the end of the last track's stream
    // seconds before the listener hears it and then does nothing but wait for
    // the queue to drain -- and a seek arriving there used to be read by nobody.
    // The slider moved, the position obediently followed, and the track went on
    // ending. There is no next track here, so nothing was played from the wrong
    // place; the seek simply never happened.
    constexpr int kFrames = static_cast<int>(kSampleRate) * 6;

    const auto file = makeFlac("tail_seek", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kSampleRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, 8.0);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));

    // Past the point where the decoder has certainly finished: the queue holds
    // about three and a half seconds, so end of stream is reached by the time a
    // little over two have been heard.
    bool waiting = false;
    for (int i = 0; i < 1000; ++i) {
        if (engine.trackPositionSeconds() >= 4.0) {
            waiting = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(waiting);

    REQUIRE(engine.seek(1.0));
    engine.waitUntilFinished();
    engine.stop();

    // Four seconds heard, then five more from one second in. Unserviced, the
    // capture is the track's own six.
    const std::size_t frames = capturedAudio(*output).size() / kChannels;
    CHECK(frames > static_cast<std::size_t>(kSampleRate) * 7);
}
