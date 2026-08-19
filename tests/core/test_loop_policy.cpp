// Whether a looping format is allowed to play for ever.
//
// Sequenced, chiptune and emulated formats mostly do not end -- what ends them
// is a length and a fade this player invents. Repeat-one is the listener saying
// they did not want that, and the override is for everything that is not a
// listener: a converter asking for a track wants the track, not an endless
// stream, and would silently get one because whoever ran it left repeat-one on.
//
// The plumbing is what is tested here: that the answer is derived from the same
// setting Cog reads, that it reaches a decoder through the registry, and that
// the override wins. Whether each individual decoder then acts on it is a
// property of that decoder.

#include "xpcog/core/LoopPolicy.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

using namespace xpcog;

namespace {

/// Records what the registry told it, and answers the question a real looping
/// decoder would ask.
class PolicyProbe final : public IDecoder {
public:
    static inline LoopPolicy lastPolicy  = LoopPolicy::Player;
    static inline bool       lastForever = false;

    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource*) override {
        lastPolicy  = loopPolicy();
        lastForever = loopForever(settings_);
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override { return {}; }
    bool readAudio(AudioChunk&) override { return false; }
    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}

private:
    const Settings* settings_ = nullptr;
};

/// A second claimant on the same extension, so MultiDecoder is in the path.
class DecliningDecoder final : public IDecoder {
public:
    bool open(ISource*) override { return false; }
    [[nodiscard]] TrackProperties properties() const override { return {}; }
    bool readAudio(AudioChunk&) override { return false; }
    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}
};

class NullSource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool seekable() const override { return true; }
    bool seek(std::int64_t, int) override { return true; }
    [[nodiscard]] std::int64_t tell() const override { return 0; }
    std::int64_t read(void*, std::int64_t) override { return 0; }
    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }

private:
    Url url_;
};

constexpr std::string_view kScheme[]    = {"probe"};
constexpr std::string_view kExtension[] = {"loop"};
constexpr std::string_view kMulti[]     = {"multi"};

void populate(PluginRegistry& registry) {
    registry.addSource({
        .name    = "NullSource",
        .schemes = kScheme,
        .create  = []() -> SourcePtr { return std::make_unique<NullSource>(); },
    });
    registry.addDecoder({
        .name       = "PolicyProbe",
        .extensions = kExtension,
        .create     = []() -> DecoderPtr { return std::make_unique<PolicyProbe>(); },
    });
    // Two claimants on "multi", so selection goes through MultiDecoder. The
    // higher-priority one declines, leaving the probe to win.
    registry.addDecoder({
        .name       = "DecliningDecoder",
        .priority   = 5.0F,
        .extensions = kMulti,
        .create     = []() -> DecoderPtr { return std::make_unique<DecliningDecoder>(); },
    });
    registry.addDecoder({
        .name       = "PolicyProbeMulti",
        .priority   = 1.0F,
        .extensions = kMulti,
        .create     = []() -> DecoderPtr { return std::make_unique<PolicyProbe>(); },
    });
    registry.freeze();
}

struct Fixture {
    std::unique_ptr<ISettingsStore> store    = makeMemorySettingsStore();
    Settings                        settings{*store};
    PluginRegistry                  registry;

    Fixture() {
        populate(registry);
        registry.setSettings(&settings);
    }

    void setRepeat(RepeatMode mode) { settings.setRepeatMode(static_cast<int>(mode)); }
};

}  // namespace

TEST_CASE("looping follows the player's repeat-one setting", "[loop]") {
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    // The same setting key Cog's IsRepeatOneSet() reads, and only that value.
    settings.setRepeatMode(static_cast<int>(RepeatMode::One));
    CHECK(loopsForever(&settings, LoopPolicy::Player));

    for (const RepeatMode mode : {RepeatMode::None, RepeatMode::Album, RepeatMode::All}) {
        settings.setRepeatMode(static_cast<int>(mode));
        CHECK_FALSE(loopsForever(&settings, LoopPolicy::Player));
    }

    // Repeating the whole playlist is not repeating this track: the next thing
    // has to get a turn, so the track still has to end.
    settings.setRepeatMode(static_cast<int>(RepeatMode::All));
    CHECK_FALSE(loopsForever(&settings, LoopPolicy::Player));
}

TEST_CASE("the never-loop override beats the player's setting", "[loop]") {
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setRepeatMode(static_cast<int>(RepeatMode::One));

    // What a converter or a disk writer asks for. Without it, the file it wrote
    // would be endless because whoever ran it had repeat-one switched on.
    CHECK_FALSE(loopsForever(&settings, LoopPolicy::Never));

    // And a decoder with no settings at all cannot be repeating anything.
    CHECK_FALSE(loopsForever(nullptr, LoopPolicy::Player));
}

TEST_CASE("the registry hands the policy to the decoder", "[loop]") {
    Fixture fixture;
    fixture.setRepeat(RepeatMode::One);
    const Url url = *Url::parse("probe://x/y.loop");

    SECTION("playback gets the player's answer") {
        PolicyProbe::lastForever = false;
        REQUIRE(fixture.registry.open(url));
        CHECK(PolicyProbe::lastPolicy == LoopPolicy::Player);
        CHECK(PolicyProbe::lastForever);
    }

    SECTION("a caller that needs an ending says so") {
        PolicyProbe::lastForever = true;
        REQUIRE(fixture.registry.open(url, SkipCue::No, LoopPolicy::Never));
        CHECK(PolicyProbe::lastPolicy == LoopPolicy::Never);
        CHECK_FALSE(PolicyProbe::lastForever);
    }
}

TEST_CASE("the policy survives decoder selection", "[loop]") {
    // MultiDecoder creates the winning candidate itself, so the policy has to be
    // forwarded rather than only set on the wrapper -- otherwise every format
    // with more than one claimant silently reverts to the default.
    Fixture fixture;
    fixture.setRepeat(RepeatMode::One);
    const Url url = *Url::parse("probe://x/y.multi");

    PolicyProbe::lastForever = true;
    REQUIRE(fixture.registry.open(url, SkipCue::No, LoopPolicy::Never));
    CHECK(PolicyProbe::lastPolicy == LoopPolicy::Never);
    CHECK_FALSE(PolicyProbe::lastForever);
}

TEST_CASE("looping is answered live, not latched at open", "[loop]") {
    // The listener can switch repeat-one on part-way through a piece of game
    // music and expects the fade to stop coming, which is why the decoders ask
    // per read rather than remembering what they were told.
    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setRepeatMode(static_cast<int>(RepeatMode::All));
    CHECK_FALSE(loopsForever(&settings, LoopPolicy::Player));

    settings.setRepeatMode(static_cast<int>(RepeatMode::One));
    CHECK(loopsForever(&settings, LoopPolicy::Player));

    settings.setRepeatMode(static_cast<int>(RepeatMode::All));
    CHECK_FALSE(loopsForever(&settings, LoopPolicy::Player));
}
