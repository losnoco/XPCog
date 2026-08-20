// That the embedded resources hold what AppIcon.cpp and LucideIcon.cpp claim.
//
// Both failures this covers are invisible by inspection, and both survive to a
// shipped binary.
//
// A missing icon size: a toolkit answers a request for a size it does not have by
// scaling one it does, and reports nothing. So a PNG renamed, dropped from the
// CMake FILES list, or regenerated at the wrong dimensions leaves an application
// that still shows an icon -- just a blurry one at whichever sizes went missing,
// which is the tray and the title bar, the two places nobody screenshots.
//
// A missing Lucide glyph: lucideIcon() answers an unknown name with an empty
// bundle, and an empty bundle on a toolbar button draws as nothing at all. The
// button is still there, still clickable, and blank.
//
// Read as bytes rather than by building bitmaps, because decoding needs the image
// handlers and therefore an application object, which this binary deliberately
// does not have. The substance is the same: both functions are loops over exactly
// these names.
//
// The header-signature check is what replaces the old suite's QImage width and
// height assertions. A PNG's IHDR carries its dimensions in bytes 16..23, big
// endian, which is enough to catch a master regenerated at the wrong size without
// decoding anything.

#include "AppIcon.hpp"
#include "LucideIcon.hpp"

#include "icons_resources.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;
using xpcog::app::applicationIconPath;
using xpcog::app::applicationIconSizes;
using xpcog::app::lucideIconNames;
using xpcog::app::lucideIconPath;

namespace {

[[nodiscard]] std::string_view asText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The width and height out of a PNG's IHDR, plus whether it carries alpha,
/// without decoding anything.
struct PngSize {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    bool          hasAlpha = false;
};

[[nodiscard]] PngSize readPngHeader(std::span<const std::byte> bytes) {
    PngSize size;
    if (bytes.size() < 26) {
        return size;
    }
    const auto at = [&bytes](std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
    };
    const auto be32 = [&at](std::size_t index) {
        return (at(index) << 24) | (at(index + 1) << 16) | (at(index + 2) << 8) |
               at(index + 3);
    };
    size.width  = be32(16);
    size.height = be32(20);

    // Transparency comes two ways, and this set uses both -- which is worth
    // knowing rather than asserting away. Colour type is byte 25: 4 is
    // grey+alpha and 6 is RGB+alpha, which is six of the seven masters. The
    // 16px one is colour type 3, a palette, whose transparency lives in a tRNS
    // chunk instead; make-icons.py's optimiser picks that encoding because at
    // that size a palette is smaller. Both are real alpha, and checking only the
    // colour type would fail on the one size that matters most -- the tray and
    // the title bar.
    const std::uint32_t colourType = at(25);
    if (colourType == 4 || colourType == 6) {
        size.hasAlpha = true;
        return size;
    }
    if (colourType != 3) {
        return size;
    }

    // Walk the chunk list for tRNS. Each chunk is a big-endian length, a
    // four-byte name, the data, and a CRC.
    for (std::size_t pos = 8; pos + 12 <= bytes.size();) {
        const std::uint32_t length = be32(pos);
        const std::string_view name =
            asText(bytes.subspan(pos + 4, 4));
        if (name == "tRNS") {
            size.hasAlpha = true;
            return size;
        }
        if (name == "IEND") {
            break;
        }
        pos += 12 + static_cast<std::size_t>(length);
    }
    return size;
}

}  // namespace

TEST_CASE("every advertised icon size is embedded, at that size", "[wx][icon]") {
    for (const int size : applicationIconSizes()) {
        const std::string path = applicationIconPath(size);
        INFO("icon resource " << path);

        const std::span<const std::byte> bytes = resources::icons(path);
        REQUIRE_FALSE(bytes.empty());

        const PngSize header = readPngHeader(bytes);
        // Exactly square at the advertised size. A resize that let the aspect
        // ratio drift would still load and still look roughly right in the About
        // box, while being subtly wrong everywhere the icon is drawn to a square.
        CHECK(header.width == static_cast<std::uint32_t>(size));
        CHECK(header.height == static_cast<std::uint32_t>(size));
        // The artwork is transparent outside the gear. Without an alpha channel
        // the tray and the title bar show a black or white square around it.
        CHECK(header.hasAlpha);
    }
}

TEST_CASE("the icon set spans the sizes the small cases need", "[wx][icon]") {
    // 16 is the tray and the title bar, 32 the taskbar, 256 what Explorer wants
    // for a large view. Losing the ends of the range is the regression that
    // matters: the middle sizes scale from each other tolerably, the ends do not.
    const std::vector<int> sizes = applicationIconSizes();
    const auto             has   = [&sizes](int size) {
        return std::find(sizes.begin(), sizes.end(), size) != sizes.end();
    };
    CHECK(has(16));
    CHECK(has(32));
    CHECK(has(256));
}

TEST_CASE("every Lucide icon the app names is embedded", "[wx][icon]") {
    const std::vector<std::string> names = lucideIconNames();
    // A guard against the list itself being emptied by a bad edit, which would
    // make every check below vacuously pass.
    REQUIRE(names.size() >= 11);

    for (const std::string& name : names) {
        INFO("lucide icon " << name);

        const std::span<const std::byte> bytes = resources::icons(lucideIconPath(name));
        REQUIRE_FALSE(bytes.empty());

        const std::string_view svg = asText(bytes);
        CHECK(svg.find("<svg") != std::string_view::npos);
        // The hook the colour is applied through. Without it the icon still
        // draws -- in whatever colour upstream chose, which is the bug: a dark
        // theme gets a toolbar of black on near-black.
        CHECK(svg.find("currentColor") != std::string_view::npos);
    }
}

TEST_CASE("the icon names are unique", "[wx][icon]") {
    // Two entries for one name would make the count above look healthier than it
    // is, and a duplicate is what a copy-pasted line leaves behind.
    const std::vector<std::string> names = lucideIconNames();
    const std::set<std::string>    unique(names.begin(), names.end());
    CHECK(unique.size() == names.size());
}

TEST_CASE("asking for a resource that is not there returns nothing", "[wx][icon]") {
    // The behaviour every check above depends on. If a missing name came back as
    // some other file's bytes, none of this would be testing what it claims.
    CHECK(resources::icons("lucide/no-such-icon.svg").empty());
    CHECK(resources::icons("").empty());
}
