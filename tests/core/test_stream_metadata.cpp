// The stream-metadata seam: a source that learns tags mid-play reaches the
// delegate, whichever decoder happens to be on top of it.
//
// This is the design point where XPCog deliberately differs from Cog, which
// polls inside each decoder -- five of them, each downcasting to HTTPSource, and
// the rest silently dropping titles. The engine polls the source instead, so the
// test drives a fake decoder that knows nothing about streams and asserts the
// tags still arrive.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace xpcog;

namespace {

constexpr double kRate     = 44100.0;
constexpr int    kChannels = 2;

/// How many polls the fake source stays silent before announcing a title, so the
/// announcement lands mid-track rather than degenerating into "metadata at
/// open()", which is the path every file already exercises.
constexpr int kQuietPolls = 3;

int gPollCount = 0;

class FakeStreamSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool seekable() const override { return false; }
    bool seek(std::int64_t, int) override { return false; }
    [[nodiscard]] std::int64_t tell() const override { return 0; }
    std::int64_t read(void*, std::int64_t) override { return 0; }
    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }

    [[nodiscard]] MetadataMap takeUpdatedMetadata() override {
        if (++gPollCount != kQuietPolls) {
            return {};
        }
        MetadataMap tags;
        tags.set("artist", "Live Artist");
        tags.set("title", "Live Title");
        return tags;
    }

private:
    Url url_;
};

/// Emits a fixed number of silent chunks and ignores its source entirely, the
/// way a decoder written before streams existed would.
class FakeDecoder final : public IDecoder {
public:
    bool open(ISource*) override { return true; }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate    = kRate;
        props.format.channels      = kChannels;
        props.format.channelConfig = 0x3;  // FL | FR
        props.format.format        = SampleFormat::F32;
        props.totalFrames          = chunksLeft_ * kFrames;
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
        std::byte* dst = out.allocFrames(kFrames);
        std::memset(dst, 0, kFrames * kChannels * sizeof(float));
        out.setFrameCount(kFrames);
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}

private:
    static constexpr std::size_t kFrames = 4096;
    // Long enough to outlast play()'s priming, which swallows the first sixteen
    // chunks of a stereo track before the feeder loop -- where the polling
    // lives -- ever runs. A track that ends inside priming is never polled.
    std::int64_t chunksLeft_ = 64;
};

// --- shutdown fakes ------------------------------------------------------
//
// A live stream whose server has gone quiet: the connection is up, the decoder
// asked for bytes, and none are coming. Parking in read() is what HttpSource
// genuinely does there, and it is the state stop() has to be able to end.

std::mutex              gParkMutex;
std::condition_variable gParkCv;
bool                    gUnblocked = false;
std::atomic<bool>       gParked{false};

void unblockParkedReads() {
    {
        const std::lock_guard lock(gParkMutex);
        gUnblocked = true;
    }
    gParkCv.notify_all();
}

class ParkingSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool seekable() const override { return false; }
    bool seek(std::int64_t, int) override { return false; }
    [[nodiscard]] std::int64_t tell() const override { return served_; }

    std::int64_t read(void* out, std::int64_t bytes) override {
        std::unique_lock lock(gParkMutex);
        if (served_ < kServeBytes && !gUnblocked) {
            const std::int64_t take = std::min(bytes, kServeBytes - served_);
            std::memset(out, 0, static_cast<std::size_t>(take));
            served_ += take;
            return take;
        }
        gParked.store(true);
        gParkCv.wait(lock, [] { return gUnblocked; });
        return -1;
    }

    void close() override { unblockParkedReads(); }
    [[nodiscard]] const Url& url() const override { return url_; }
    void interrupt() override { unblockParkedReads(); }

private:
    /// Enough to outlast play()'s priming, so the park happens in the feeder
    /// loop -- the thread stop() joins -- rather than before it starts.
    static constexpr std::int64_t kServeBytes = 1 << 20;

    Url          url_;
    std::int64_t served_ = 0;
};

/// Gates every chunk on a real read, so the source's block reaches the feeder.
class GatedDecoder final : public IDecoder {
public:
    bool open(ISource* source) override {
        source_ = source;
        return source_ != nullptr;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate    = kRate;
        props.format.channels      = kChannels;
        props.format.channelConfig = 0x3;
        props.format.format        = SampleFormat::F32;
        return props;
    }

    bool readAudio(AudioChunk& out) override {
        char gate[1024];
        if (source_->read(gate, sizeof(gate)) <= 0) {
            return false;
        }

        AudioFormat format;
        format.sampleRate    = kRate;
        format.channels      = kChannels;
        format.channelConfig = 0x3;
        format.format        = SampleFormat::F32;
        out.setFormat(format);
        std::byte* dst = out.allocFrames(kFrames);
        std::memset(dst, 0, kFrames * kChannels * sizeof(float));
        out.setFrameCount(kFrames);
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}
    void interrupt() override {}

private:
    static constexpr std::size_t kFrames = 4096;
    ISource* source_ = nullptr;
};

constexpr std::string_view kParkScheme[] = {"park"};
constexpr std::string_view kParkExt[]    = {"live"};

void populateParkingRegistry(PluginRegistry& registry) {
    registry.addSource({
        .name    = "ParkingSource",
        .schemes = kParkScheme,
        .create  = []() -> SourcePtr { return std::make_unique<ParkingSource>(); },
    });
    registry.addDecoder({
        .name       = "GatedDecoder",
        .extensions = kParkExt,
        .create     = []() -> DecoderPtr { return std::make_unique<GatedDecoder>(); },
    });
    registry.freeze();
}

/// Refuses to open, the way a corrupt file or an unreachable stream does.
class BrokenDecoder final : public IDecoder {
public:
    bool open(ISource*) override { return false; }
    [[nodiscard]] TrackProperties properties() const override { return {}; }
    bool readAudio(AudioChunk&) override { return false; }
    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}
};

constexpr std::string_view kScheme[]    = {"fake"};
constexpr std::string_view kExtension[] = {"tst"};
constexpr std::string_view kBadExt[]    = {"bad"};

void populateFakeRegistry(PluginRegistry& registry) {
    registry.addSource({
        .name    = "FakeStreamSource",
        .schemes = kScheme,
        .create  = []() -> SourcePtr { return std::make_unique<FakeStreamSource>(); },
    });
    registry.addDecoder({
        .name       = "FakeDecoder",
        .extensions = kExtension,
        .create     = []() -> DecoderPtr { return std::make_unique<FakeDecoder>(); },
    });
    registry.addDecoder({
        .name       = "BrokenDecoder",
        .extensions = kBadExt,
        .create     = []() -> DecoderPtr { return std::make_unique<BrokenDecoder>(); },
    });
    registry.freeze();
}

struct RecordingDelegate final : AudioEngine::Delegate {
    std::vector<std::pair<std::string, MetadataMap>> updates;

    void streamMetadataChanged(const Url& url, const MetadataMap& tags) override {
        updates.emplace_back(url.toString(), tags);
    }
};

}  // namespace

TEST_CASE("tags a source learns mid-play reach the delegate", "[engine][stream]") {
    gPollCount = 0;

    PluginRegistry registry;
    populateFakeRegistry(registry);

    RingBuffer ring(static_cast<std::size_t>(kRate * 0.5) * kChannels);
    auto       output = makeOfflineOutput(ring);
    auto       store  = makeMemorySettingsStore();
    Settings   settings(*store);

    AudioEngine       engine(registry, *output, ring, settings);
    RecordingDelegate delegate;
    engine.setDelegate(&delegate);

    const Url url = *Url::parse("fake://radio.example/stream.tst");
    REQUIRE(engine.play(url));
    engine.waitUntilFinished();

    // Exactly one: the source announced once, and an empty answer must not be
    // reported as a change.
    REQUIRE(delegate.updates.size() == 1);
    CHECK(delegate.updates[0].first == url.toString());
    CHECK(delegate.updates[0].second.first("artist") == "Live Artist");
    CHECK(delegate.updates[0].second.first("title") == "Live Title");

    // And the source really was polled more often than it answered.
    CHECK(gPollCount > kQuietPolls);
}


TEST_CASE("a repeating playlist of undecodable tracks ends instead of spinning",
          "[engine][stream]") {
    // Repeat-one over a file that cannot be decoded: the delegate answers the
    // same URL for ever, exactly as PlaybackController does when the playlist's
    // repeat rules say so. The engine must notice the candidate coming round
    // again and stop, rather than retrying at full speed and flooding
    // trackFailed -- which is what it did before the failed-candidate check in
    // the advance loop.
    gPollCount = 0;

    PluginRegistry registry;
    populateFakeRegistry(registry);

    RingBuffer ring(static_cast<std::size_t>(kRate * 0.5) * kChannels);
    auto       output = makeOfflineOutput(ring);
    auto       store  = makeMemorySettingsStore();
    Settings   settings(*store);

    AudioEngine engine(registry, *output, ring, settings);

    struct RepeatOneDelegate final : AudioEngine::Delegate {
        Url  repeated;
        int  failures = 0;
        std::optional<Url> nextTrack() override { return repeated; }
        void trackFailed(const Url&) override { ++failures; }
    } delegate;
    delegate.repeated = *Url::parse("fake://radio.example/broken.bad");
    engine.setDelegate(&delegate);

    REQUIRE(engine.play(*Url::parse("fake://radio.example/stream.tst")));
    // The proof is that this returns at all.
    engine.waitUntilFinished();

    // Failed once, was offered again, and the engine declined the second try.
    CHECK(delegate.failures == 1);
}


TEST_CASE("stopping a stream parked in read() does not deadlock",
          "[engine][stream]") {
    // The freeze behind "it hangs when I switch songs": switching calls stop(),
    // stop() joins the feeder, and the feeder is inside a read that is waiting
    // for stream data that will never arrive. running_ = false cannot reach a
    // blocked read, and closeTrack() -- which would end it -- runs after the
    // join. ISource::interrupt() exists for exactly this and was called by
    // nothing.
    using namespace std::chrono_literals;

    {
        const std::lock_guard lock(gParkMutex);
        gUnblocked = false;
    }
    gParked.store(false);

    PluginRegistry registry;
    populateParkingRegistry(registry);

    RingBuffer ring(static_cast<std::size_t>(kRate * 0.5) * kChannels);
    auto       output = makeOfflineOutput(ring);
    auto       store  = makeMemorySettingsStore();
    Settings   settings(*store);

    AudioEngine engine(registry, *output, ring, settings);
    REQUIRE(engine.play(*Url::parse("park://radio.example/stream.live")));

    // Wait for the feeder to genuinely block, so this cannot pass by stopping
    // before the interesting state exists.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!gParked.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(gParked.load());

    auto stopped = std::async(std::launch::async, [&engine] { engine.stop(); });
    const auto status = stopped.wait_for(15s);

    // Rescue the blocked thread before asserting: a future's destructor waits,
    // so without this a failure would hang the suite instead of reporting.
    if (status != std::future_status::ready) {
        unblockParkedReads();
        stopped.wait();
    }
    CHECK(status == std::future_status::ready);
}
