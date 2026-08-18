// Settings reaching a codec.
//
// A descriptor's create() takes no arguments, so the registry hands settings
// over immediately after construction -- the same shape as setRegistry(). The
// thing worth testing is that every path does it, because the failure mode is
// silent: a codec that never receives them falls back to Cog's defaults and
// plays perfectly well, just deaf to the preference someone changed.
//
// The MultiDecoder case is the one that would rot unnoticed. It builds its
// candidates lazily inside open(), one at a time until one succeeds, so the
// registry never sees them.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

using namespace xpcog;

namespace {

const Settings* gSourceSaw  = nullptr;
const Settings* gDecoderSaw = nullptr;
int             gDecodersBuilt = 0;

class RecordingSource final : public ISource {
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

    void setSettings(const Settings* settings) override { gSourceSaw = settings; }

private:
    Url url_;
};

/// `opens` decides whether this candidate succeeds, so a MultiDecoder can be
/// made to walk past the first one.
template <bool Opens>
class RecordingDecoder final : public IDecoder {
public:
    RecordingDecoder() { ++gDecodersBuilt; }

    bool open(ISource*) override { return Opens; }
    [[nodiscard]] TrackProperties properties() const override { return {}; }
    bool readAudio(AudioChunk&) override { return false; }
    std::int64_t seek(std::int64_t) override { return -1; }
    void close() override {}

    void setSettings(const Settings* settings) override { gDecoderSaw = settings; }
};

constexpr std::string_view kScheme[] = {"set"};
constexpr std::string_view kExt[]    = {"one"};
constexpr std::string_view kMultiExt[] = {"two"};

void reset() {
    gSourceSaw     = nullptr;
    gDecoderSaw    = nullptr;
    gDecodersBuilt = 0;
}

}  // namespace

TEST_CASE("the registry hands settings to the codecs it builds", "[registry][settings]") {
    reset();

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    PluginRegistry registry;
    registry.setSettings(&settings);
    registry.addSource({
        .name    = "RecordingSource",
        .schemes = kScheme,
        .create  = []() -> SourcePtr { return std::make_unique<RecordingSource>(); },
    });
    registry.addDecoder({
        .name       = "RecordingDecoder",
        .extensions = kExt,
        .create     = []() -> DecoderPtr {
            return std::make_unique<RecordingDecoder<true>>();
        },
    });
    registry.freeze();

    const auto opened = registry.open(*Url::parse("set://host/file.one"));
    REQUIRE(opened);

    CHECK(gSourceSaw == &settings);
    CHECK(gDecoderSaw == &settings);
}

TEST_CASE("a MultiDecoder passes settings to each candidate it tries",
          "[registry][settings]") {
    reset();

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    PluginRegistry registry;
    registry.setSettings(&settings);
    registry.addSource({
        .name    = "RecordingSource",
        .schemes = kScheme,
        .create  = []() -> SourcePtr { return std::make_unique<RecordingSource>(); },
    });

    // Two claim the extension, so the registry wraps them. The higher-priority
    // one refuses to open, which is what makes the second one get built at all.
    registry.addDecoder({
        .name       = "Refuses",
        .priority   = 2.0F,
        .extensions = kMultiExt,
        .create     = []() -> DecoderPtr {
            return std::make_unique<RecordingDecoder<false>>();
        },
    });
    registry.addDecoder({
        .name       = "Accepts",
        .priority   = 1.0F,
        .extensions = kMultiExt,
        .create     = []() -> DecoderPtr {
            return std::make_unique<RecordingDecoder<true>>();
        },
    });
    registry.freeze();

    const auto opened = registry.open(*Url::parse("set://host/file.two"));
    REQUIRE(opened);

    // Both were built -- so the second really did go through the lazy path --
    // and the last one to be told is the one that ended up playing.
    CHECK(gDecodersBuilt == 2);
    CHECK(gDecoderSaw == &settings);
}

TEST_CASE("a registry with no settings still builds working codecs",
          "[registry][settings]") {
    // Tests and the headless tools never set any, and a codec must fall back to
    // its own defaults rather than dereferencing what it was handed.
    reset();

    PluginRegistry registry;
    registry.addSource({
        .name    = "RecordingSource",
        .schemes = kScheme,
        .create  = []() -> SourcePtr { return std::make_unique<RecordingSource>(); },
    });
    registry.addDecoder({
        .name       = "RecordingDecoder",
        .extensions = kExt,
        .create     = []() -> DecoderPtr {
            return std::make_unique<RecordingDecoder<true>>();
        },
    });
    registry.freeze();

    CHECK(registry.settings() == nullptr);
    REQUIRE(registry.open(*Url::parse("set://host/file.one")));
    CHECK(gSourceSaw == nullptr);
    CHECK(gDecoderSaw == nullptr);
}
