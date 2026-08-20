// The SC-55 panel's one dependency that can fail silently.
//
// The front-panel photograph is compiled in, and something compiled into a
// *static* library is exactly the kind of thing that vanishes without a word: the
// linker drops what nothing references, which is the same rule that makes
// self-registering codecs disappear (see cmake/XPCogCodec.cmake). When it goes,
// the widget draws nothing at all and looks like a synchronisation bug.
//
// The generated lookup makes that hazard smaller than it was under Qt's resource
// system rather than merely as small: `resources::sc55()` is a function, so a call
// to it is an undefined symbol the linker has to resolve, which pulls the whole
// translation unit -- data and all -- in with it. A .qrc registered by a static
// initialiser had nothing referring to it at all.
//
// No widget here, and none is needed. What is being checked is that the bytes are
// reachable and are the shape the emulator will be handed.

#include "sc55_resources.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace {

/// 741 x 268 pixels of RGBA, which is what lcd_background_size comes to.
constexpr std::size_t kBackgroundBytes = 741U * 268U * 4U;

}  // namespace

TEST_CASE("the SC-55 front panel image is compiled in", "[wx][sc55]") {
    const std::span<const std::byte> bytes = xpcog::resources::sc55("back.data");
    REQUIRE_FALSE(bytes.empty());
    // The exact size, not merely non-empty: the panel renderer indexes into this
    // by row and column, so a truncated or differently-shaped image is a read past
    // the end rather than a picture that looks wrong.
    CHECK(bytes.size() == kBackgroundBytes);
}

TEST_CASE("a name the SC-55 resource does not hold comes back empty", "[wx][sc55]") {
    // The behaviour the check above depends on. If a missing name answered with
    // some other file's bytes, the size assertion would be testing the wrong
    // thing.
    CHECK(xpcog::resources::sc55("front.data").empty());
}
