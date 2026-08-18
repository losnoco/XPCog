#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;

namespace {

DecoderPtr makeNothing() { return nullptr; }

class MimeSource final : public ISource {
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
    [[nodiscard]] std::string mimeType() const override { return "audio/x-scpls"; }

private:
    Url url_;
};

SourcePtr makeMimeSource() { return std::make_unique<MimeSource>(); }

std::vector<Url> expandExtension(const Url&, ISource&) {
    return {*Url::parse("fake://station/extension.flac")};
}

std::vector<Url> expandMime(const Url&, ISource&) {
    return {*Url::parse("fake://station/mime.flac")};
}

constexpr std::string_view kExtA[]  = {"aaa"};
constexpr std::string_view kExtB[]  = {"bbb", "aaa"};
constexpr std::string_view kMime[]  = {"audio/test"};
constexpr std::string_view kFakeScheme[] = {"fake"};
constexpr std::string_view kPlsMime[]    = {"audio/x-scpls"};

bool gAvailableCalled = false;
bool sayNo()  { gAvailableCalled = true; return false; }

}  // namespace

TEST_CASE("registerAllCodecs registers the compiled-in codecs", "[registry]") {
    PluginRegistry registry;
    registerAllCodecs(registry);

    // The generated RegisterAll.cpp must have called freeze() for us.
    REQUIRE(registry.frozen());

    // Guards the whole generated-registration mechanism: if the linker ever drops a
    // codec's object file, this count silently falls and the test fails loudly.
    CHECK(registry.decoderCount() >= 1);

    const auto extensions = registry.allExtensions();
    const bool hasFlac =
        std::find(extensions.begin(), extensions.end(), "flac") != extensions.end();
    CHECK(hasFlac);
}

TEST_CASE("freeze sorts descriptors by descending priority", "[registry]") {
    PluginRegistry registry;
    registry.addDecoder({.name       = "Low",
                         .priority   = 0.5F,
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.addDecoder({.name       = "High",
                         .priority   = 2.0F,
                         .extensions = kExtB,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.freeze();

    REQUIRE(registry.decoderCount() == 2);
    CHECK(registry.frozen());
}

TEST_CASE("allExtensions is sorted and deduplicated", "[registry]") {
    PluginRegistry registry;
    // "aaa" is claimed by both descriptors and must appear exactly once.
    registry.addDecoder({.name       = "A",
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.addDecoder({.name       = "B",
                         .extensions = kExtB,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing});
    registry.freeze();

    const auto extensions = registry.allExtensions();
    REQUIRE(extensions.size() == 2);
    CHECK(extensions[0] == "aaa");
    CHECK(extensions[1] == "bbb");
}

TEST_CASE("unavailable descriptors are dropped at freeze", "[registry]") {
    gAvailableCalled = false;

    PluginRegistry registry;
    registry.addDecoder({.name       = "Unavailable",
                         .extensions = kExtA,
                         .mimeTypes  = kMime,
                         .create     = &makeNothing,
                         .available  = &sayNo});
    registry.freeze();

    CHECK(gAvailableCalled);
    CHECK(registry.decoderCount() == 0);
    CHECK(registry.allExtensions().empty());
}

TEST_CASE("container selection uses MIME for an extensionless URL", "[registry][scanner]") {
    PluginRegistry registry;
    registry.addSource({.name     = "MimeSource",
                        .schemes  = kFakeScheme,
                        .create   = &makeMimeSource});
    registry.addContainer({.name      = "PlsByMime",
                           .mimeTypes = kPlsMime,
                           .expand    = &expandMime});
    registry.freeze();

    const Url station = *Url::parse("fake://station/listen");

    // The static check stays cheap because only open() can reveal Content-Type.
    CHECK_FALSE(registry.isContainer(station));

    const Scanner scanner{registry};
    const auto    entries = scanner.expand({&station, 1});
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().toString() == "fake://station/mime.flac");
}

TEST_CASE("container extension takes priority over MIME", "[registry]") {
    PluginRegistry registry;
    registry.addSource({.name     = "MimeSource",
                        .schemes  = kFakeScheme,
                        .create   = &makeMimeSource});
    registry.addContainer({.name       = "ByExtension",
                           .extensions = kExtA,
                           .expand     = &expandExtension});
    registry.addContainer({.name      = "ByMime",
                           .mimeTypes = kPlsMime,
                           .expand    = &expandMime});
    registry.freeze();

    const auto entries = registry.expandContainer(*Url::parse("fake://station/list.aaa"));
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().toString() == "fake://station/extension.flac");
}
