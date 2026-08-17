#include "xpcog/core/PluginRegistry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

using namespace xpcog;

namespace {

DecoderPtr makeNothing() { return nullptr; }

constexpr std::string_view kExtA[]  = {"aaa"};
constexpr std::string_view kExtB[]  = {"bbb", "aaa"};
constexpr std::string_view kMime[]  = {"audio/test"};

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
