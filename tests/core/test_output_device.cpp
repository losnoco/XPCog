// Moving playback to another output device without restarting the track.
//
// The device is opened when a track starts, so a device or share-mode change
// used to mean re-opening the track and seeking back -- honest, but a gap of
// however long the file takes to open rather than however long the driver does.
// Switching under the running stream keeps both rings where the format allows
// it, so the new device resumes at the very next sample; where it does not, the
// rings are dropped and the decoder rewound to the frame last heard, so the gap
// gains a seek and the track is still never re-opened.
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

#include "xpcog/core/Plugin.hpp"
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

/// The same, for a capture taken from a device running some other format --
/// which is the whole subject of the tests at the bottom of this file, and the
/// reason the divisor cannot be a constant.
double secondsOf(const std::vector<float>& captured, double rate,
                 std::uint32_t channels) {
    return static_cast<double>(captured.size()) / (rate * channels);
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

/// The rate a shared-mode device is already running, and will not move off.
constexpr double kSharedRate = 32000.0;

/// An output that converts behind the seam unless it is held exclusively --
/// which is what miniaudio does, and the bug effectiveSampleRate() exists for.
///
/// It answers every rate to supportsSampleRate(), because the rate genuinely
/// can be *requested*; what it will not do is run at one. That gap is the whole
/// problem: before this question existed the engine asked for the track's rate,
/// was told yes, built its converter against it, and the backend quietly
/// resampled with something worse than soxr while negotiatedFormat() reported
/// the rate that had been asked for.
///
/// Exclusive is modelled the way the hardware measured: an exclusive stream owns
/// the device and switches it, so it really does run whatever it is handed.
class SharedModeOutput final : public IAudioOutput {
public:
    SharedModeOutput(std::unique_ptr<IAudioOutput> inner, double sharedRate)
        : inner_(std::move(inner)), sharedRate_(sharedRate) {}

    [[nodiscard]] double effectiveSampleRate(double wanted, std::string_view deviceId,
                                             bool exclusive) const override {
        askedWanted_    = wanted;
        askedDeviceId_  = std::string{deviceId};
        askedExclusive_ = exclusive;
        asked_          = true;
        return exclusive ? wanted : sharedRate_;
    }

    [[nodiscard]] bool wasAsked() const { return asked_; }
    [[nodiscard]] double askedWanted() const { return askedWanted_; }
    [[nodiscard]] std::string askedDeviceId() const { return askedDeviceId_; }
    [[nodiscard]] bool askedExclusive() const { return askedExclusive_; }
    [[nodiscard]] double openedAt() const { return openedAt_; }

    bool start(const Config& config) override {
        openedAt_ = config.sampleRate;
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

private:
    std::unique_ptr<IAudioOutput> inner_;
    double                        sharedRate_;
    mutable double                askedWanted_    = 0.0;
    mutable std::string           askedDeviceId_;
    mutable bool                  askedExclusive_ = false;
    mutable bool                  asked_          = false;
    double                        openedAt_       = 0.0;
};

/// The rate and width a reshaping device runs at, whatever it is asked for.
/// Both far enough from the fixtures' own that a wrong answer is a wrong number
/// rather than a rounding argument.
constexpr double        kDeviceRate     = 32000.0;
constexpr std::uint32_t kDeviceChannels = 1;

/// An output whose chosen device runs one format and one only.
///
/// Real hardware does this by having a fixed clock -- the exclusive-mode DAC
/// that runs 48,000 and nothing else, the mono USB interface -- and from the
/// engine's side there is nothing else to it: start() succeeds and
/// negotiatedFormat() answers something other than what was asked. Only a named
/// device reshapes, so the empty-string default the engine falls back to still
/// runs whatever it is given, which is what makes it a *different* device.
class ReshapingOutput final : public IAudioOutput {
public:
    ReshapingOutput(std::unique_ptr<IAudioOutput> inner, double rate,
                    std::uint32_t channels)
        : inner_(std::move(inner)), rate_(rate), channels_(channels) {}

    bool start(const Config& config) override {
        Config reshaped = config;
        if (!config.deviceId.empty()) {
            reshaped.sampleRate = rate_;
            reshaped.channels   = channels_;
        }
        return inner_->start(reshaped);
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
    double                        rate_;
    std::uint32_t                 channels_;
};

/// A source with nothing to read and a decoder that will not seek: between them,
/// live radio. The point of interest is `seekable`, which is false by default in
/// TrackProperties and is what decides whether a format change can be followed.
class UnseekableSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool         seekable() const override { return false; }
    bool                       seek(std::int64_t, int) override { return false; }
    [[nodiscard]] std::int64_t tell() const override { return 0; }
    std::int64_t               read(void*, std::int64_t) override { return 0; }
    void                       close() override {}
    [[nodiscard]] const Url&   url() const override { return url_; }

private:
    Url url_;
};

class UnseekableDecoder final : public IDecoder {
public:
    bool open(ISource*) override { return true; }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate    = kRate;
        props.format.channels      = kChannels;
        props.format.channelConfig = 0x3;  // FL | FR
        props.format.format        = SampleFormat::F32;
        props.totalFrames          = kChunks * static_cast<std::int64_t>(kFrames);
        props.seekable             = false;
        return props;
    }

    bool readAudio(AudioChunk& out) override {
        if (chunksLeft_ == 0) {
            return false;
        }
        --chunksLeft_;

        AudioFormat format;
        format.sampleRate    = kRate;
        format.channels      = kChannels;
        format.channelConfig = 0x3;
        format.format        = SampleFormat::F32;
        out.setFormat(format);
        auto* dst = reinterpret_cast<float*>(out.allocFrames(kFrames));
        for (std::size_t i = 0; i < kFrames * kChannels; ++i) {
            dst[i] = 0.0F;
        }
        out.setFrameCount(kFrames);
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void         close() override {}

private:
    static constexpr std::size_t  kFrames = 4096;
    static constexpr std::int64_t kChunks = 128;  // ~12s at 44.1 kHz stereo
    std::int64_t                  chunksLeft_ = kChunks;
};

constexpr std::string_view kLiveScheme[] = {"live"};
constexpr std::string_view kLiveExt[]    = {"live"};

void populateLiveRegistry(PluginRegistry& registry) {
    registry.addSource({
        .name    = "UnseekableSource",
        .schemes = kLiveScheme,
        .create  = []() -> SourcePtr { return std::make_unique<UnseekableSource>(); },
    });
    registry.addDecoder({
        .name       = "UnseekableDecoder",
        .extensions = kLiveExt,
        .create     = []() -> DecoderPtr { return std::make_unique<UnseekableDecoder>(); },
    });
    registry.freeze();
}

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


TEST_CASE("a shared device is opened at the rate it really runs", "[device]") {
    // The bug this closes: miniaudio accepts a rate it has no intention of
    // running, keeps it in device.sampleRate, runs the hardware at
    // internalSampleRate, and puts a *linear* resampler between the two. Nothing
    // downstream could see it, because negotiatedFormat() reported the rate that
    // had been asked for -- so AudioConverter compared two equal numbers,
    // correctly did nothing, and soxr sat unused above a linear resampler.
    //
    // The engine now asks what the device will really do, before it builds
    // anything that has to agree about the rate.
    constexpr int kFrames = static_cast<int>(kRate) * 2;

    const auto file = makeFlac("shared-rate", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer       ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    SharedModeOutput output{makeOfflineOutput(ring, kSpeed), kSharedRate};

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setOutputDeviceId("offline");
    settings.setOutputExclusive(false);

    AudioEngine engine{registry(), output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(*file)));

    // Asked at all, about the device that was going to be opened, and about the
    // track's own rate rather than something already substituted.
    REQUIRE(output.wasAsked());
    CHECK(output.askedWanted() == kRate);
    CHECK(output.askedDeviceId() == "offline");
    CHECK_FALSE(output.askedExclusive());

    // And the answer was used. Both numbers, because they answer different
    // questions: openedAt() is what the engine asked the device for, and
    // negotiatedFormat() is what came back -- and the point of the change is
    // that the first one moved.
    CHECK(output.openedAt() == kSharedRate);
    CHECK(output.negotiatedFormat().sampleRate == kSharedRate);

    // Still plays. The conversion did not vanish, it moved: 44,100 to 32,000 is
    // now AudioConverter's work with soxr, which is where it belongs.
    engine.waitUntilFinished();
    engine.stop();
}

TEST_CASE("an exclusive device is opened at the track's rate", "[device]") {
    // The half that must not regress, and the reason effectiveSampleRate() takes
    // the share mode rather than answering one way for everything. An exclusive
    // stream owns the hardware and switches it -- measured on a real device with
    // tools/ma-rate-probe, where every rate asked for in exclusive mode came back
    // as the internal rate. Converting into the mix rate there would throw away
    // bit-perfect playback to solve a problem that mode does not have.
    //
    // It is also what keeps DoP reachable: its carrier has to be running at
    // exactly 176,400 or 352,800, and an output path that resampled it would put
    // the markers out as noise.
    constexpr int kFrames = static_cast<int>(kRate) * 2;

    const auto file = makeFlac("exclusive-rate", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer       ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    SharedModeOutput output{makeOfflineOutput(ring, kSpeed), kSharedRate};

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setOutputDeviceId("offline");
    settings.setOutputExclusive(true);

    AudioEngine engine{registry(), output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(*file)));

    REQUIRE(output.wasAsked());
    CHECK(output.askedExclusive());

    // The track's rate, untouched -- not the shared device's.
    CHECK(output.openedAt() == kRate);
    CHECK(output.negotiatedFormat().sampleRate == kRate);

    engine.waitUntilFinished();
    engine.stop();
}

TEST_CASE("an output that answers nothing keeps the rate it was given",
          "[device]") {
    // The default implementation returns `wanted`, which is the truth for an
    // output that really does run whatever it is handed -- OfflineOutput, and
    // every test double that does not override this. Pinned because the
    // alternative reading of "0 means no answer" would substitute a rate for one
    // of them and quietly resample the whole test suite.
    constexpr int kFrames = static_cast<int>(kRate) * 2;

    const auto file = makeFlac("plain-rate", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring, kSpeed);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    AudioEngine engine{registry(), *output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    CHECK(output->negotiatedFormat().sampleRate == kRate);

    engine.waitUntilFinished();
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

// --- following a device to another format --------------------------------
//
// Everything above holds the format still, which is the case where the queued
// audio is still meaningful and nothing is lost. These are the other one: the
// chosen device runs a rate or a width of its own, so what is queued is the
// wrong shape and has to go. The stream follows anyway -- the pipeline is
// re-pointed and the decoder rewound to the frame that was last audible -- so
// the audio comes back rather than being skipped, and the track is never
// re-opened.

TEST_CASE("a device that runs another rate takes the stream with it", "[device]") {
    constexpr int kFrames = static_cast<int>(kRate) * 8;

    const auto file = makeFlac("switch-rate", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer      ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    ReshapingOutput output{makeOfflineOutput(ring, kSpeed), kDeviceRate, kChannels};

    auto              store = makeMemorySettingsStore();
    Settings          settings{*store};
    RecordingDelegate delegate;
    AudioEngine       engine{registry(), output, ring, settings};
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 2.0; }));
    REQUIRE(output.negotiatedFormat().sampleRate == kRate);

    const double switchedAt = engine.trackPositionSeconds();
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    // The device really did move, and really did land somewhere else.
    REQUIRE(waitFor([&] { return output.negotiatedFormat().sampleRate == kDeviceRate; }));

    // Not a failure. The whole point of this case is that the caller is not
    // asked to re-open the track: a report here would send PlaybackController
    // down the restart-and-seek path this exists to avoid.
    CHECK(delegate.switchFailures() == 0);

    // And the clock did not jump. It is denominated in device frames and a
    // device frame is now a different length of time, so every recorded count
    // had to be rescaled with the format -- get that wrong and the position
    // reads 44100/32000 of where it was, which is a jump of most of a second.
    CHECK(engine.trackPositionSeconds() >= switchedAt - 0.5);
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() > switchedAt + 1.0; }));

    engine.waitUntilFinished();
    engine.stop();

    // The remainder of the track, in the new device's units. Too much means the
    // track was opened again from the top, which is the whole thing being
    // avoided; too little means the queue was thrown away without rewinding to
    // what was in it, and that is the *deep* ring -- about three seconds.
    //
    // The lower bound carries the offline output's own artifact, the one
    // remainderIsIntact() explains: stopping it drains the shallow ring into a
    // capture that starting the next device then clears, and counts those frames
    // as played -- so the rewind lands after audio no capture ever shows. A real
    // device is merely uninitialised and leaves the ring for its successor.
    const double aRing = static_cast<double>(ring.capacity()) / (kRate * kChannels);
    const double played =
        secondsOf(capturedAudio(output.capture()), kDeviceRate, kChannels);
    INFO("switched at " << switchedAt << "s, new device played " << played << "s");
    CHECK(played > 8.0 - switchedAt - aRing - 0.25);
    CHECK(played < 8.0 - switchedAt + 0.25);
}

TEST_CASE("a device that runs another width takes the stream with it", "[device]") {
    // The other half of the format, and not the same code path: the rate moves
    // the clock and the channel count moves the DSP chain, which is prepared for
    // a width and reads it once per pass. A pump left at the old width reads the
    // ring in the wrong stride, which is silent, wrong, and does not stop.
    constexpr int kFrames = static_cast<int>(kRate) * 6;

    const auto file = makeFlac("switch-width", kFrames);
    if (!file) {
        SKIP("flac is not installed");
    }

    RingBuffer      ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    ReshapingOutput output{makeOfflineOutput(ring, kSpeed), kRate, kDeviceChannels};

    auto              store = makeMemorySettingsStore();
    Settings          settings{*store};
    RecordingDelegate delegate;
    AudioEngine       engine{registry(), output, ring, settings};
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(Url::fromLocalPath(*file)));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 1.5; }));

    const double switchedAt = engine.trackPositionSeconds();
    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    REQUIRE(waitFor([&] { return output.negotiatedFormat().channels == kDeviceChannels; }));
    CHECK(delegate.switchFailures() == 0);

    // The rate is unchanged here, so the clock needed no rescaling and the
    // position may be held to a tighter bound than the case above.
    CHECK(engine.trackPositionSeconds() >= switchedAt - 0.25);
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() > switchedAt + 1.0; }));

    engine.waitUntilFinished();
    engine.stop();

    // Bounded as above, and short by the same shallow ring for the same reason.
    const double aRing = static_cast<double>(ring.capacity()) / (kRate * kChannels);
    const double played =
        secondsOf(capturedAudio(output.capture()), kRate, kDeviceChannels);
    INFO("switched at " << switchedAt << "s, new device played " << played << "s");
    CHECK(played > 6.0 - switchedAt - aRing - 0.25);
    CHECK(played < 6.0 - switchedAt + 0.25);
}

TEST_CASE("a stream that cannot be rewound declines the format change", "[device]") {
    // Following a device to another format costs the queue, and what pays that
    // back is rewinding the decoder to the frame last heard. A live stream
    // cannot be rewound, so there is nothing to pay with: the honest outcome is
    // to stay on the device that was working and let the caller decide, which is
    // exactly what happened for *every* format change before this.
    PluginRegistry live;
    populateLiveRegistry(live);

    RingBuffer      ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    ReshapingOutput output{makeOfflineOutput(ring, kSpeed), kDeviceRate, kChannels};

    auto              store = makeMemorySettingsStore();
    Settings          settings{*store};
    RecordingDelegate delegate;
    AudioEngine       engine{live, output, ring, settings};
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(*Url::parse("live://radio.example/stream.live")));
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() >= 1.0; }));

    settings.setOutputDeviceId("offline");
    REQUIRE(engine.switchOutputDevice());

    REQUIRE(waitFor([&] { return delegate.switchFailures() == 1; }));

    // Still on the device that was running, and still at the format it was
    // running -- the reshaping device was opened, found unusable, and closed.
    CHECK(output.negotiatedFormat().sampleRate == kRate);

    const double before = engine.trackPositionSeconds();
    REQUIRE(waitFor([&] { return engine.trackPositionSeconds() > before + 0.5; }));

    engine.stop();
}
