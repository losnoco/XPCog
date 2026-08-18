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

#include <cstring>
#include <string>
#include <string_view>
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
