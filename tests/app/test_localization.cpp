// The catalogue, from the .po on disk to something wxWidgets will answer with.
//
// Two things are being pinned down here and only one of them is arithmetic.
//
// The arithmetic is the `.mo` image. wxMsgCatalog can be built from a file or
// from a block of bytes in gettext's binary format and from nothing else, so a
// catalogue compiled into the binary has to be assembled into that format at
// startup -- two index tables of (length, offset) pairs over a run of
// NUL-terminated strings, with the lengths deliberately *not* counting the
// terminator because a plural message stores its forms with NULs inside them.
// Get an offset wrong by one and nothing crashes: the catalogue loads, and some
// arbitrary subset of the interface comes out in the wrong language or in
// fragments. That is not a failure anyone traces back to an index table, which
// is why it is checked here rather than by looking at a window.
//
// The other thing is that the build actually produced a catalogue. `es.po` is
// parsed by a CMake script, and the failure mode of a parser is not usually a
// build error -- it is an empty table, or one message where there should be
// three hundred.

#include "Localization.hpp"

#include "catalogs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/buffer.h>
#include <wx/translation.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using xpcog::app::assembleCatalog;
using xpcog::app::availableLanguages;
using xpcog::app::Catalog;
using xpcog::app::CatalogEntry;
using xpcog::app::catalogs;
using xpcog::app::LanguageOption;

namespace {

[[nodiscard]] std::uint32_t readLittleEndian(std::string_view image, std::size_t at) {
    REQUIRE(at + 4 <= image.size());
    return static_cast<std::uint32_t>(static_cast<unsigned char>(image[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(image[at + 3])) << 24);
}

/// One (length, offset) pair out of an index table, as the string it names.
[[nodiscard]] std::string_view stringAt(std::string_view image, std::uint32_t table,
                                        std::uint32_t index) {
    const std::uint32_t length = readLittleEndian(image, table + (index * 8));
    const std::uint32_t offset = readLittleEndian(image, table + (index * 8) + 4);
    REQUIRE(static_cast<std::size_t>(offset) + length + 1 <= image.size());
    // The terminator is outside the reported length and has to be there: a
    // reader that takes the pointer as a C string -- which is what wx does for
    // the msgid -- would otherwise run into the next message.
    REQUIRE(image[offset + length] == '\0');
    return image.substr(offset, length);
}

[[nodiscard]] const Catalog& spanish() {
    const std::span<const Catalog> all   = catalogs();
    const auto                     found = std::find_if(
        all.begin(), all.end(), [](const Catalog& catalog) {
            return std::string_view{catalog.language} == "es";
        });
    REQUIRE(found != all.end());
    return *found;
}

/// The catalogue, through wx's own parser.
[[nodiscard]] std::unique_ptr<wxMsgCatalog> load(const Catalog& catalog) {
    const std::string image = assembleCatalog(catalog);
    wxCharBuffer      bytes(image.size());
    std::memcpy(bytes.data(), image.data(), image.size());
    return std::unique_ptr<wxMsgCatalog>(
        wxMsgCatalog::CreateFromData(bytes, "xpcog-test"));
}

}  // namespace

TEST_CASE("the assembled catalogue is a well-formed .mo", "[wx][locale]") {
    const std::string image = assembleCatalog(spanish());
    const std::string_view view{image};

    CHECK(readLittleEndian(view, 0) == 0x950412DEU);  // little-endian magic
    CHECK(readLittleEndian(view, 4) == 0U);           // format revision

    const std::uint32_t count      = readLittleEndian(view, 8);
    const std::uint32_t originals  = readLittleEndian(view, 12);
    const std::uint32_t translated = readLittleEndian(view, 16);

    // The hash table is optional; a size of zero is how the format declines it,
    // and wx builds a hash map of its own regardless.
    CHECK(readLittleEndian(view, 20) == 0U);

    REQUIRE(count == spanish().entries.size());
    CHECK(originals == 28);
    CHECK(translated == originals + (count * 8));

    // Every pair has to resolve, which is what catches an offset that has drifted
    // by a string's terminator.
    for (std::uint32_t i = 0; i < count; ++i) {
        static_cast<void>(stringAt(view, originals, i));
        CHECK_FALSE(stringAt(view, translated, i).empty());
    }
}

TEST_CASE("the header comes first and names the charset", "[wx][locale]") {
    const std::string      image = assembleCatalog(spanish());
    const std::string_view view{image};
    const std::uint32_t    originals  = readLittleEndian(view, 12);
    const std::uint32_t    translated = readLittleEndian(view, 16);

    // Entry zero, because the entries are sorted by key and the header's key is
    // the empty string. wx reads the charset out of exactly this entry, and
    // without it every accented character in the catalogue is decoded through
    // whatever the machine's current encoding happens to be.
    CHECK(stringAt(view, originals, 0).empty());
    CHECK(stringAt(view, translated, 0).find("charset=UTF-8") != std::string_view::npos);
    CHECK(stringAt(view, translated, 0).find("nplurals=2") != std::string_view::npos);
}

TEST_CASE("the entries are sorted by key", "[wx][locale]") {
    // The format requires it so that a reader may binary search. wx builds a
    // hash map instead and would not notice -- which is the reason to check:
    // an image only the lenient parser can read is not the format it claims.
    const std::string      image = assembleCatalog(spanish());
    const std::string_view view{image};
    const std::uint32_t    count     = readLittleEndian(view, 8);
    const std::uint32_t    originals = readLittleEndian(view, 12);

    for (std::uint32_t i = 1; i < count; ++i) {
        CHECK(stringAt(view, originals, i - 1) < stringAt(view, originals, i));
    }
}

TEST_CASE("wxWidgets reads back what the build compiled in", "[wx][locale]") {
    const std::unique_ptr<wxMsgCatalog> catalog = load(spanish());
    REQUIRE(catalog);

    // A message from each of the three ways one reaches the catalogue: `_()`,
    // wxTRANSLATE through a table, and a heading that belongs to core.
    const wxString* preferences = catalog->GetString("Preferences");
    REQUIRE(preferences != nullptr);
    CHECK(*preferences == wxString::FromUTF8("Preferencias"));

    const wxString* file = catalog->GetString("&File");
    REQUIRE(file != nullptr);
    CHECK(*file == wxString::FromUTF8("&Archivo"));

    const wxString* length = catalog->GetString("Length");
    REQUIRE(length != nullptr);
    CHECK(*length == wxString::FromUTF8("Duraci\xC3\xB3n"));
}

TEST_CASE("a msgid that is not ASCII is still found", "[wx][locale]") {
    // The bug the `trUtf8()` half of app/src/Text.hpp exists for. `_()` lets its
    // literal convert to a wxString implicitly, which on Windows reads it
    // through the current 8-bit locale -- so a msgid carrying a `×` is looked
    // up under a key two mangled characters long, misses, and is then *shown*
    // mangled. Nothing about it fails to compile.
    //
    // The key here is built with FromUTF8 exactly as trUtf8() builds it, so this
    // asserts the whole path: the .po's bytes, the generated table, the
    // assembled image, and the lookup.
    const std::unique_ptr<wxMsgCatalog> catalog = load(spanish());
    REQUIRE(catalog);

    const wxString* reset =
        catalog->GetString(wxString::FromUTF8("Reset to 1.00\xC3\x97"));
    REQUIRE(reset != nullptr);
    CHECK(*reset == wxString::FromUTF8("Volver a 1,00\xC3\x97"));

    const wxString* dash =
        catalog->GetString(wxString::FromUTF8("Rubber Band \xE2\x80\x94 Finer"));
    REQUIRE(dash != nullptr);
    CHECK(*dash == wxString::FromUTF8("Rubber Band: m\xC3\xA1s fino"));

    // And the same key read the wrong way finds nothing, which is what the
    // mojibake was: a miss, followed by the mangled original being displayed.
    // Spelled with an explicit Latin-1 conversion rather than with FromAscii,
    // which asserts on a non-ASCII byte in a debug build -- the very check that
    // would have caught this had the broken path gone through it. The implicit
    // conversion `_()` uses does not.
    CHECK(catalog->GetString(wxString("Reset to 1.00\xC3\x97", wxConvISO8859_1)) ==
          nullptr);
}

TEST_CASE("a plural message keeps both of its forms", "[wx][locale]") {
    const std::unique_ptr<wxMsgCatalog> catalog = load(spanish());
    REQUIRE(catalog);

    // Keyed on the singular msgid, with the form chosen by the rule in the
    // header. This is the case the NUL-joined value exists for, and the one an
    // off-by-one in the length would break while leaving every ordinary message
    // working.
    const wxString* one = catalog->GetString("Add %zu Track", 1);
    REQUIRE(one != nullptr);
    CHECK(*one == wxString::FromUTF8("a\xC3\xB1"
                                     "adir %zu pista"));

    const wxString* many = catalog->GetString("Add %zu Track", 3);
    REQUIRE(many != nullptr);
    CHECK(*many == wxString::FromUTF8("a\xC3\xB1"
                                      "adir %zu pistas"));

    // Zero is plural in Spanish, as it is in English.
    const wxString* none = catalog->GetString("Add %zu Track", 0);
    REQUIRE(none != nullptr);
    CHECK(*none == *many);
}

TEST_CASE("every compiled catalogue holds real messages", "[wx][locale]") {
    // The parser in cmake/CompileCatalog.cmake fails by producing an empty
    // table, not by producing an error, so the count is the assertion.
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

TEST_CASE("the picker offers the system, English and every catalogue",
          "[wx][locale]") {
    const std::vector<LanguageOption> options = availableLanguages();
    REQUIRE(options.size() == catalogs().size() + 2);

    // The system entry first, and its code is empty rather than "en": storing a
    // language for somebody who asked to follow the desktop would pin them to
    // whatever it happened to be on the day they looked at the row.
    CHECK(options.front().code.empty());
    CHECK(options[1].code == "en");

    for (const Catalog& catalog : catalogs()) {
        const auto found =
            std::find_if(options.begin(), options.end(),
                         [&catalog](const LanguageOption& option) {
                             return option.code == catalog.language;
                         });
        INFO("catalogue: " << catalog.language);
        REQUIRE(found != options.end());
        // The language's own name for itself, so it is findable by somebody who
        // cannot read the interface they are currently looking at.
        CHECK_FALSE(found->name.empty());
        CHECK(found->name != found->code);
    }
}
