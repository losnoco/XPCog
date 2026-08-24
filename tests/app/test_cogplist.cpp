// Reading a defaults file that is in Apple's binary format.
//
// Here rather than in the core suite because it is the one part of the Cog
// import that needs an OS framework: `org.cogx.cog.plist` is `bplist00`, and
// only CoreFoundation reads that. Everything the conversion feeds -- the mapping
// from Cog's keys to XPCog's -- is in core and tested there, on every platform.
//
// The fixture plist is written in binary on purpose, by
// tools/cogimport-fixture/make-fixture.py, so this exercises the real path
// rather than a convenient XML stand-in.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/CogImport.hpp"
#include "xpcog/platform/PropertyListFile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace xpcog;
using Catch::Approx;

namespace {

[[nodiscard]] std::filesystem::path fixturePlist() {
    return std::filesystem::path{XPCOG_COG_FIXTURE_DIR} / "org.cogx.cog.plist";
}

}  // namespace

TEST_CASE("the fixture defaults really are binary", "[cogplist]") {
    // The premise of everything below. If this ever reads "<?xml", the fixture
    // generator has changed format and the case that follows has quietly stopped
    // testing CoreFoundation.
    std::ifstream file{fixturePlist(), std::ios::binary};
    REQUIRE(file);
    std::string header(8, '\0');
    file.read(header.data(), 8);
    CHECK(header == "bplist00");
}

TEST_CASE("a binary defaults file converts and imports", "[cogplist]") {
    const auto xml = platform::propertyListToXml(fixturePlist());

#if defined(__APPLE__)
    REQUIRE(xml.has_value());
    CHECK(xml->find("<?xml") != std::string::npos);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    CogSettingsReport report;
    importCogSettings(*xml, settings, &report);

    // Sixteen keys: eleven XPCog shares, five it does not. The five are four
    // Cog-only settings (metadataMigrated, miniPlusMode, and the pitch/tempo
    // pair that belong to Rubber Band, which is not ported) and one of AppKit's
    // window frames.
    //
    // It read ten and six until GraphicEQenable had somewhere to land. The
    // previous version of this comment said "if that changes this case should
    // say 11 and 5", which it now does -- the arithmetic here is a census of how
    // much of a Cog installation XPCog can actually accept, so it is supposed to
    // move when that changes, and supposed to be looked at when it does.
    CHECK(report.applied == 11);
    CHECK(report.ignored == 5);
    CHECK(report.mismatched == 0);

    // Spot-checked across the three value shapes, since the conversion is where
    // a binary plist could differ from the XML the core tests use.
    CHECK(settings.Volume() == Approx(78.5));
    CHECK(settings.RepeatMode() == 2);
    CHECK(settings.Eq20Hz() == Approx(3.5));
    CHECK(settings.Eq1kHz() == Approx(-2.0));
    CHECK(settings.GraphicEqPreset() == 7);
    CHECK(settings.GraphicEqEnable());
    CHECK(settings.SentryAskedConsent());
#else
    // Documented behaviour rather than an accident: only CoreFoundation reads
    // the binary format, and answering nullopt is what this promises off macOS.
    // Asserted so that a future binary plist reader in core makes this case fail
    // and be reconsidered, rather than silently going unused.
    CHECK_FALSE(xml.has_value());
#endif
}

TEST_CASE("a file that is not a property list converts to nothing",
          "[cogplist]") {
    CHECK_FALSE(platform::propertyListToXml("/nonexistent/none.plist").has_value());
    CHECK_FALSE(platform::propertyListToXml(
                    std::filesystem::path{XPCOG_COG_FIXTURE_DIR} / "DataModel.sqlite")
                    .has_value());
}
