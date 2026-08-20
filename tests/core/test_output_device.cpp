// Moving playback to another output device without restarting the track.
//
// The device is opened when a track starts, so a device or share-mode change
// used to mean re-opening the track and seeking back -- honest, but a gap of
// however long the file takes to open rather than however long the driver does.
// Switching under the running stream keeps both rings, so the new device
// resumes at the very next sample.
//
// What that costs is the position clock. Every position the engine records is an
// absolute count of frames delivered, and a device counts from zero again the
// moment it is started -- so a switch that did nothing else would send the seek
// bar back to the top of the track and leave it there. These tests are mostly
// about that number.
//
// The offline output is a fair double here for once: what is being checked is
// which samples came out and what the clock said, neither of which needs a sound
// card. It is a poor one for the *gap*, which is the driver's and is not
// simulated -- that part is a listening judgement on real hardware.

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate     = 44100.0;
constexpr int    kChannels = 2;

/// Fast enough that a test finishes in well under a second, slow enough that a
/// switch requested part-way through lands part-way through. Unpaced, an
/// eight-second file is consumed in the time it takes to decode it and there is
/// no "part-way" to act at -- the same trap test_seek.cpp records.
constexpr double kSpeed = 8.0;

std::filesystem::path fixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-device-tests";
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
        const double t = static_cast<double>(i) / kRate;
        const auto   v = static_cast<std::int16_t>(
            20000.0 * std::sin(xpcog::test::kTwoPi * 440.0 * t));
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
    u32(static_cast<std::uint32_t>(kRate));
    u32(static_cast<std::uint32_t>(kRate) * kChannels * 2);
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

/// Spins until `predicate` holds or the deadline passes. Returns whether it held.
template <typename Predicate>
bool waitFor(Predicate predicate, int milliseconds = 4000) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 5) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// Seconds of stereo audio in a captured buffer.
double secondsOf(const std::vector<float>& captured) {
    return static_cast<double>(captured.size()) / (kRate * kChannels);
}

/// Whether `played` is the rest of an eight-second track from `switchedAt`.
///
/// Two bounds, and each is a different way for a switch to be wrong. Too much
/// means the track was re-opened and played from the top again, which is the
/// whole thing being avoided. Too little means the audio already queued for the
/// old device was thrown away rather than handed on.
///
/// The slack below is the offline output's, not the engine's: stopping it
/// drains the whole shallow ring into a capture that starting the next device
/// then clears, so the new capture is short by up to a ring. A real device is
/// merely uninitialised and leaves the ring where it is for its successor.
/// That difference is also why the *gap* a switch leaves is a listening
/// judgement on real hardware rather than something asserted here.
bool remainderIsIntact(double played, double switchedAt, const RingBuffer& ring) {
    const double remaining = 8.0 - switchedAt;
    const double aRing = static_cast<double>(ring.capacity()) / (kRate * kChannels);
    return played <= remaining + 0.25 && played >= remaining - aRing - 0.25;
}

/// An output that can be told to refuse its next start or two, wrapped around a
/// real offline one so everything else behaves.
///
/// Both ways a live switch can fail run through start() returning false, and
/// neither is reachable otherwise: refusing the first attempt is a device that
/// will not open, and refusing the second as well is that plus the device that
/// was playing going away underneath it. The second is the one that used to
/// deadlock the feeder, so it is worth being able to reach on purpose rather
/// than by unplugging something.
class FlakyOutput final : public IAudioOutput {
public:
    explicit FlakyOutput(std::unique_ptr<IAudioOutput> inner)
        : inner_(std::move(inner)) {}

    /// Applies to the next `count` starts, from any thread.
    void refuseNextStarts(int count) {
        refusals_.store(count, std::memory_order_relaxed);
    }

    bool start(const Config& config) override {
        int outstanding = refusals_.load(std::memory_order_relaxed);
        while (outstanding > 0 &&
               !refusals_.compare_exchange_weak(outstanding, outstanding - 1,
                                                std::memory_order_relaxed)) {
        }
        if (outstanding > 0) {
            return false;
        }
        return inner_->start(config);
    }

    void stop() override { inner_->stop(); }
    void pause() override { inner_->pause(); }
    void resume() override { inner_->resume(); }

    [[nodiscard]] AudioFormat negotiatedFormat() const override {
        return inner_->negotiatedFormat();
    }
    [[nodiscard]] double latencySeconds() const override {
        return inner_->latencySeconds();
    }
    [[nodiscard]] std::vector<DeviceInfo> devices() const override {
        return inner_->devices();
    }
    void  setVolume(float gain) override { inner_->setVolume(gain); }
    [[nodiscard]] float volume() const override { return inner_->volume(); }
    void rampGain(float target, double milliseconds) override {
        inner_->rampGain(target, milliseconds);
    }
    [[nodiscard]] bool ramping() const override { return inner_->ramping(); }
    [[nodiscard]] std::uint64_t underrunCount() const override {
        return inner_->underrunCount();
    }
    [[nodiscard]] std::uint64_t framesPlayed() const override {
        return inner_->framesPlayed();
    }
    void setDeviceInvalidatedCallback(std::function<void()> callback) override {
        inner_->setDeviceInvalidatedCallback(std::move(callback));
    }
    void setTap(AudioTap* tap) override { inner_->setTap(tap); }

    [[nodiscard]] const IAudioOutput& capture() const { return *inner_; }

private:
    std::unique_ptr<IAudioOutput> inner_;
    std::atomic<int>              refusals_{0};
};

/// The rate every track in this file is refused at, and the one the fussy
/// output below offers instead. Well under 44,100 so an ordinary fixture
/// provokes the fallback.
constexpr double kOnlyRate = 22050.0;

/// An output that refuses the track's own sample rate, so the engine has to ask
/// what it would rather run at -- and records which device it was asked about.
///
/// Refusing 44,100 rather than the DSD rates that provoke this in earnest is
/// what keeps the case runnable anywhere: the question is which device the
/// engine names, and that is the same question whatever rate raised it.
class RateFussyOutput final : public IAudioOutput {
public:
    explicit RateFussyOutput(std::unique_ptr<IAudioOutput> inner)
        : inner_(std::move(inner)) {}

    [[nodiscard]] bool supportsSampleRate(double sampleRate) const override {
        return sampleRate <= kOnlyRate;
    }

    /// Not synchronised, and it does not need to be: the engine asks this from
    /// inside play(), on the caller's thread, before the feeder exists.
    [[nodiscard]] double preferredSampleRate(std::string_view deviceId) const override {
        askedAbout_ = std::string{deviceId};
        asked_      = true;
        return kOnlyRate;
    }

    [[nodiscard]] bool        wasAsked() const { return asked_; }
    [[nodiscard]] std::string askedAbout() const { return askedAbout_; }

    bool start(const Config& config) override { return inner_->start(config); }
    void stop() override { inner_->stop(); }
    void pause() override { inner_->pause(); }
    void resume() override { inner_->resume(); }

    [[nodiscard]] AudioFormat negotiatedFormat() const override {
        return inner_->negotiatedFormat();
    }
    [[nodiscard]] double latencySeconds() const override {
        return inner_->latencySeconds();
    }
    [[nodiscard]] std::vector<DeviceInfo> devices() const override {
        return inner_->devices();
    }
    void  setVolume(float gain) override { inner_->setVolume(gain); }
    [[nodiscard]] float volume() const override { return inner_->volume(); }
    void rampGain(float target, double milliseconds) override {
        inner_->rampGain(target, milliseconds);
    }
    [[nodiscard]] bool ramping() const override { return inner_->ramping(); }
    [[nodiscard]] std::uint64_t underrunCount() const override {
        return inner_->underrunCount();
    }
    [[nodiscard]] std::uint64_t framesPlayed() const override {
        return inner_->framesPlayed();
    }
    void setDeviceInvalidatedCallback(std::function<void()> callback) override {
        inner_->setDeviceInvalidatedCallback(std::move(callback));
    }
    void setTap(AudioTap* tap) override { inner_->setTap(tap); }

private:
    std::unique_ptr<IAudioOutput> inner_;
    mutable std::string           askedAbout_;
    mutable bool                  asked_ = false;
};

/// Counts the delegate calls the failure paths are supposed to make.
class RecordingDelegate final : public AudioEngine::Delegate {
public:
    void outputSwitchFailed() override {
        switchFailed_.fetch_add(1, std::memory_order_relaxed);
    }
    void stoppedNaturally() override {
        stopped_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] int switchFailures() const {
        return switchFailed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int stops() const {
        return stopped_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int> switchFailed_{0};
    std::atomic<int> stopped_{0};
};

}  // namespace

TEST_CASE("the position clock survives a live device switch", "[device]") {
    // The whole point. framesPlayed() returns to zero when a device starts, and
    // everything the engine records is measured against it -- so without a base
    // to carry the difference, the seek bar jumps to the top of the track the
    // instant the device changes and counts up again from there.
    constexpr int kFrames = static_cast<int>(kRate) * 8;

    const auto file = makeFlac("switch-position", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, kSpeed);

    auto        store = makeMemorySettingsStore();
    Settings    settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    // Nothing playing has no device to move.
    REQUIRE_FALSE(engine.switchOutputDevice());

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 2.0; }));

    const double      before   = engine.trackPositionSeconds();
    const std::size_t captured = capturedAudio(*output).size();

    // The offline output names one device, which is enough to be a different
    // answer from the empty string play() opened with.
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    // That a device really did change, before anything is claimed about the
    // clock across it: the offline output clears its capture whenever one
    // starts, so a capture that has got *shorter* is the witness. Without this
    // the test passes on an engine that quietly ignored the request, since a
    // clock nobody disturbed keeps perfectly good time.
    REQUIRE(waitFor([&] { return capturedAudio(*output).size() < captured; }));

    // And now the point. A regression shows up as a drop to nearly zero and a
    // climb from there, so both halves are checked: still ahead of where it
    // was, and still moving.
    REQUIRE(engine.trackPositionSeconds() >= before);
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() > before + 0.5; }));

    engine.waitUntilFinished();
    engine.stop();
}

TEST_CASE("a live device switch does not start the track again", "[device]") {
    // The offline output clears its capture whenever a device starts, which is
    // exactly the lever this needs: whatever is captured after the switch is
    // what the new device played. A track that had been re-opened would deliver
    // the whole file again, and a switch that dropped the queued audio would
    // deliver noticeably less than the remainder.
    constexpr int kFrames = static_cast<int>(kRate) * 8;

    const auto file = makeFlac("switch-continues", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, kSpeed);

    auto        store = makeMemorySettingsStore();
    Settings    settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 2.0; }));

    const double switchedAt = engine.trackPositionSeconds();
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    engine.waitUntilFinished();
    engine.stop();

    const double played = secondsOf(capturedAudio(*output));
    INFO("switched at " << switchedAt << "s, new device played " << played << "s");
    REQUIRE(remainderIsIntact(played, switchedAt, ring));
}

TEST_CASE("a switch to the device already playing is not a switch", "[device]") {
    // MainWindow calls this on any write to the device keys, and a write that
    // resolves to the device already running must not interrupt anything. The
    // capture is the witness: a restart would clear it, so a full-length one
    // means the device was left alone.
    constexpr int kFrames = static_cast<int>(kRate) * 4;

    const auto file = makeFlac("switch-noop", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, kSpeed);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    // Chosen before play(), so the running device and the requested one agree.
    settings.setOutputDeviceId("offline");

    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 1.0; }));

    REQUIRE(engine.switchOutputDevice());

    engine.waitUntilFinished();
    engine.stop();

    const double played = secondsOf(capturedAudio(*output));
    INFO("captured " << played << "s of a four-second file");
    REQUIRE(played > 3.5);
}

TEST_CASE("a paused transport declines a live switch", "[device]") {
    // Not a limitation being papered over: the feeder services the switch, and
    // a paused feeder is parked waiting for room in a ring nothing is draining,
    // so the request would be read at the one moment it is no longer wanted.
    // The caller re-opens the track instead, which is what used to happen every
    // time.
    constexpr int kFrames = static_cast<int>(kRate) * 4;

    const auto file = makeFlac("switch-paused", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, kSpeed);

    auto        store = makeMemorySettingsStore();
    Settings    settings{*store};
    AudioEngine engine{registry(), *output, ring, settings};

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 0.5; }));

    engine.pause();
    settings.setOutputDeviceId("offline");
    REQUIRE_FALSE(engine.switchOutputDevice());

    engine.stop();
    REQUIRE_FALSE(engine.switchOutputDevice());
}

TEST_CASE("a device that will not open leaves playback where it was", "[device]") {
    // The honest half of switching under the stream. The engine cannot re-open
    // the track by itself -- it has never heard of the playlist -- so it puts
    // the stream back on the device that was working and says so, and the
    // caller's fallback is the restart-and-seek that used to happen every time.
    constexpr int kFrames = static_cast<int>(kRate) * 8;

    const auto file = makeFlac("switch-refused", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer  ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    FlakyOutput output{makeOfflineOutput(ring, kSpeed)};

    auto             store = makeMemorySettingsStore();
    Settings         settings{*store};
    RecordingDelegate delegate;
    AudioEngine      engine{registry(), output, ring, settings};
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 2.0; }));

    const double before = engine.trackPositionSeconds();

    // One refusal: the new device declines, the one already playing takes it
    // back.
    output.refuseNextStarts(1);
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    REQUIRE(waitFor([&] { return delegate.switchFailures() == 1; }));

    // The stream is intact, and so is the clock -- the old device restarted
    // too, so it needed re-basing just the same. That is the part a fallback
    // written for "the switch worked or nothing happened" would get wrong.
    REQUIRE(engine.trackPositionSeconds() >= before);
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() > before + 0.5; }));

    engine.waitUntilFinished();
    engine.stop();

    // And the audio came through the way it does when the switch works: putting
    // the stream back must not cost any more of it than moving it did.
    const double played = secondsOf(capturedAudio(output.capture()));
    INFO("refused at " << before << "s, old device played " << played << "s");
    REQUIRE(remainderIsIntact(played, before, ring));
}

TEST_CASE("losing every device ends the transport rather than wedging it", "[device]") {
    // The new device will not open and neither will the one that was playing,
    // which on real hardware means it has been unplugged mid-switch. Nothing
    // drains the rings after that, so every "wait until the buffers are empty"
    // in the feeder is a wait for something that cannot happen -- and there are
    // three of them. Playback has to end instead, and be seen to end.
    constexpr int kFrames = static_cast<int>(kRate) * 8;

    const auto file = makeFlac("switch-lost", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer  ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    FlakyOutput output{makeOfflineOutput(ring, kSpeed)};

    auto              store = makeMemorySettingsStore();
    Settings          settings{*store};
    RecordingDelegate delegate;
    AudioEngine       engine{registry(), output, ring, settings};
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 1.0; }));

    output.refuseNextStarts(2);
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    // waitUntilFinished() is the assertion: it returns, and it does so long
    // before the seven seconds of audio it is not going to be given.
    REQUIRE(waitFor([&] { return delegate.stops() == 1; }));
    engine.waitUntilFinished();
    REQUIRE(engine.status() == PlaybackStatus::Stopped);

    // No switch-failed report: this is not a switch that failed and left the
    // stream running, it is the end of playback.
    REQUIRE(delegate.switchFailures() == 0);
    engine.stop();
}


TEST_CASE("the refused-rate fallback asks about the chosen device", "[device]") {
    // `preferredSampleRate()` used to take no argument and answer for the
    // system default, with a comment beside it saying "since nothing selects
    // another one yet" -- which stopped being true when the device picker
    // landed. It only bites where the track's own rate was refused, so the
    // listener with a DSD DAC selected got the *default* device's mix rate and
    // a resample to it.
    constexpr int kFrames = static_cast<int>(kRate) * 2;

    const auto file = makeFlac("refused-rate", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    RateFussyOutput output{makeOfflineOutput(ring, kSpeed)};

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    // The offline output names exactly one device, so this is a selection that
    // resolves -- and one the engine must repeat back rather than substitute.
    settings.setOutputDeviceId("offline");

    AudioEngine engine{registry(), output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(*file)));

    // The premise: 44,100 was refused, so the question was actually asked.
    REQUIRE(output.wasAsked());
    CHECK(output.askedAbout() == "offline");

    // And the answer was used, rather than the 48,000 the engine falls back to
    // when the preferred rate is refused as well. Read from the output, which
    // is what the device was actually opened at.
    CHECK(output.negotiatedFormat().sampleRate == kOnlyRate);

    engine.waitUntilFinished();
    engine.stop();
}

TEST_CASE("no chosen device asks about no device", "[device]") {
    // The other half, and the reason the case above is not satisfied by an
    // implementation that always passes something: an empty selection has to
    // stay empty, because empty is what both the engine and the backend read as
    // "the system default, and it follows the system default".
    constexpr int kFrames = static_cast<int>(kRate) * 2;

    const auto file = makeFlac("refused-rate-default", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer      ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    RateFussyOutput output{makeOfflineOutput(ring, kSpeed)};

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    AudioEngine engine{registry(), output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(*file)));

    REQUIRE(output.wasAsked());
    CHECK(output.askedAbout().empty());

    engine.waitUntilFinished();
    engine.stop();
}
