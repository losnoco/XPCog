// The catalogue the build compiles in, and the lookup that reads it.
//
// `es.po` is parsed by a CMake script (cmake/CompileCatalog.cmake), and the
// failure mode of a parser is not usually a build error -- it is an empty table,
// or one message where there should be three hundred. So the table's shape is
// checked here, and then the lookup every frontend calls: tr(), trn() and the
// substituter behind trf().
//
// These cases lived in the wx player's suite, beside the ones that fed the same
// table to wxTranslations; those went with wx in 3.0.0, and these stayed,
// because nothing in them was ever about wx.

#include "Translations.hpp"
#include "catalogs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using xpcog::app::Catalog;
using xpcog::app::CatalogEntry;
using xpcog::app::catalogs;

namespace {

/// A language installed for the length of a case, and whatever was there before
/// put back after it, so no case leaves the binary speaking Spanish to the next.
class Installed {
public:
    explicit Installed(const std::string& language)
        : previous_(xpcog::app::currentLanguage()) {
        xpcog::app::installNeutralTranslations(language);
    }
    Installed(const Installed&)            = delete;
    Installed& operator=(const Installed&) = delete;
    ~Installed() { xpcog::app::installNeutralTranslations(previous_); }

private:
    std::string previous_;
};

}  // namespace

TEST_CASE("every compiled catalogue holds real messages", "[locale]") {
    // The parser fails by producing an empty table, not by producing an error,
    // so the count is the assertion.
    REQUIRE_FALSE(catalogs().empty());

    for (const Catalog& catalog : catalogs()) {
        INFO("catalogue: " << catalog.language);
        CHECK(std::string_view{catalog.language}.size() >= 2);
        CHECK(catalog.entries.size() > 100);

        bool header = false;
        for (const CatalogEntry& entry : catalog.entries) {
            REQUIRE(entry.singular != nullptr);
            REQUIRE(entry.forms[0] != nullptr);
            if (std::string_view{entry.singular}.empty() && entry.context == nullptr) {
                header = true;
                continue;
            }
            // An untranslated message is dropped by the compiler rather than
            // carried as an empty string, which is what makes a partly
            // translated .po fall back to English instead of to nothing.
            CHECK_FALSE(std::string_view{entry.forms[0]}.empty());
            // A plural msgid needs a second form, or the rule in the header has
            // nothing to choose between.
            if (entry.plural != nullptr) {
                REQUIRE(entry.forms[1] != nullptr);
                CHECK_FALSE(std::string_view{entry.forms[1]}.empty());
            }
        }
        CHECK(header);
    }
}

TEST_CASE("an installed catalogue answers tr() with its own text", "[locale]") {
    const Installed viaUs{"es"};

    // Every singular message in the table, asked for by its English and checked
    // against the form the table holds. A lookup that answered some of them --
    // an index off by one, a hash collision -- would pass a spot check.
    int compared = 0;
    for (const Catalog& catalog : catalogs()) {
        if (std::string_view{catalog.language} != "es") {
            continue;
        }
        for (const CatalogEntry& entry : catalog.entries) {
            if (entry.plural != nullptr || entry.context != nullptr ||
                std::string_view{entry.singular}.empty()) {
                continue;
            }
            INFO("msgid: " << entry.singular);
            CHECK(xpcog::app::tr(entry.singular) == entry.forms[0]);
            ++compared;
        }
    }
    // A comparison that compared nothing would pass.
    CHECK(compared > 100);
}

TEST_CASE("an untranslated message comes back as its msgid", "[locale]") {
    const Installed viaUs{"es"};

    // Not in any catalogue, and must not become empty. A lookup that answered
    // with "" would replace a perfectly good English label with nothing, which
    // is the failure cmake/CompileCatalog.cmake drops empty msgstrs to avoid.
    CHECK(xpcog::app::tr("Not a message this build has ever had") ==
          "Not a message this build has ever had");
    CHECK(xpcog::app::trn("One nonexistent thing", "Some nonexistent things", 1) ==
          "One nonexistent thing");
    CHECK(xpcog::app::trn("One nonexistent thing", "Some nonexistent things", 3) ==
          "Some nonexistent things");
}

TEST_CASE("the substituter fills specifiers in order", "[locale]") {
    using xpcog::app::fmt;

    CHECK(fmt("Add %zu Tracks", std::size_t{3}) == "Add 3 Tracks");
    CHECK(fmt("%s (remote)", std::string{"Remove 2 Tracks"}) ==
          "Remove 2 Tracks (remote)");
    CHECK(fmt("%s of %d", "one", 5) == "one of 5");
    CHECK(fmt("100%% done") == "100% done");

    // A translation that disagrees with its msgid still has to produce a
    // sentence. Neither of these is worth losing the message over, and both are
    // things a translator does by hand.
    CHECK(fmt("Add %zu Tracks and %zu more", std::size_t{3}) ==
          "Add 3 Tracks and %zu more");
    CHECK(fmt("No specifiers here", std::size_t{3}) == "No specifiers here");

    // The one that would be a hole if this were snprintf.
    CHECK(fmt("%n%s", std::string{"safe"}) == "%nsafe");
}
