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

#include <wx/arrstr.h>
#include <wx/buffer.h>
#include <wx/translation.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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

constexpr const char* kDomain = "xpcog-test";

/// Hands wxTranslations the one catalogue under test.
class OneCatalog : public wxTranslationsLoader {
public:
    explicit OneCatalog(const Catalog& catalog) : catalog_(&catalog) {}

    wxMsgCatalog* LoadCatalog(const wxString& domain, const wxString&) override {
        const std::string image = assembleCatalog(*catalog_);
        wxCharBuffer      bytes(image.size());
        std::memcpy(bytes.data(), image.data(), image.size());
        return wxMsgCatalog::CreateFromData(bytes, domain);
    }

    wxArrayString GetAvailableTranslations(const wxString&) const override {
        wxArrayString languages;
        languages.Add(wxString::FromAscii(catalog_->language));
        return languages;
    }

private:
    const Catalog* catalog_;
};

/// The catalogue, parsed by wx and queried the way the application queries it.
///
/// **wx owns the wxMsgCatalog**, and that is not tidiness -- it is what makes
/// this link. wxWidgets 3.2 does not declare `~wxMsgCatalog` at all in a Unicode
/// build, so the implicit one is inline, and it destroys a
/// `wxPluralFormsCalculatorPtr` whose own destructor lives only inside the
/// library. Deleting a catalogue from outside wx is therefore an undefined
/// reference at link time. 3.3 moved the destructor out of line, which is why a
/// `std::unique_ptr<wxMsgCatalog>` here linked on Windows and macOS and broke
/// the Linux job, which builds against the distribution's 3.2. Routing through
/// wxTranslations is also what the application does, so this now exercises the
/// loader as well as the image.
///
/// A *local* wxTranslations rather than wxTranslations::Set(), which would
/// install a Spanish catalogue for the whole binary -- test_infopanel asserts on
/// "Album Gain: ..." and would start reading its own labels in Spanish.
class Loaded {
public:
    explicit Loaded(const Catalog& catalog) {
        translations_.SetLoader(new OneCatalog(catalog));
        // Set explicitly, so the lookup never consults the machine's own
        // preferred languages -- this has to give the same answer on a Spanish
        // desktop and an English one.
        translations_.SetLanguage(wxString::FromAscii(catalog.language));
        translations_.AddCatalog(wxString::FromAscii(kDomain));
    }

    [[nodiscard]] const wxString* find(const wxString& msgid) const {
        return translations_.GetTranslatedString(msgid,
                                                 wxString::FromAscii(kDomain));
    }

    /// The plural form for `count`, chosen by the rule in the catalogue header.
    [[nodiscard]] const wxString* find(const wxString& msgid, unsigned count) const {
        return translations_.GetTranslatedString(msgid, count,
                                                 wxString::FromAscii(kDomain));
    }

private:
    wxTranslations translations_;
};

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

TEST_CASE("the entries are sorted, and each key appears once", "[wx][locale]") {
    // Sorted because the format requires it so that a reader may binary search.
    // wx builds a hash map instead and would not notice -- which is the reason
    // to check: an image only the lenient parser can read is not the format it
    // claims to be.
    //
    // Strictly ascending, so this is a uniqueness check as well, and that is the
    // half that has already earned its keep. A msgid is a key: two entries
    // under one leaves it undefined which a lookup gets. It happens the moment
    // a word this application marks for itself is also added to the catalogue
    // from somewhere else -- "Cancel" is a button on the Last.fm pane and also
    // one of wxWidgets' stock labels -- and the .po is generated, so nothing
    // upstream of here would have said so.
    const std::string      image = assembleCatalog(spanish());
    const std::string_view view{image};
    const std::uint32_t    count     = readLittleEndian(view, 8);
    const std::uint32_t    originals = readLittleEndian(view, 12);

    for (std::uint32_t i = 1; i < count; ++i) {
        const std::string_view previous = stringAt(view, originals, i - 1);
        const std::string_view current  = stringAt(view, originals, i);
        INFO("entry " << i << ": equal keys are a duplicate msgid, out-of-order "
                              "ones a broken sort");
        CHECK(previous < current);
    }
}

TEST_CASE("wxWidgets reads back what the build compiled in", "[wx][locale]") {
    const Loaded catalog{spanish()};

    // A message from each of the three ways one reaches the catalogue: `_()`,
    // wxTRANSLATE through a table, and a heading that belongs to core.
    const wxString* preferences = catalog.find("Preferences");
    REQUIRE(preferences != nullptr);
    CHECK(*preferences == wxString::FromUTF8("Preferencias"));

    const wxString* file = catalog.find("&File");
    REQUIRE(file != nullptr);
    CHECK(*file == wxString::FromUTF8("&Archivo"));

    const wxString* length = catalog.find("Length");
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
    const Loaded catalog{spanish()};

    const wxString* reset =
        catalog.find(wxString::FromUTF8("Reset to 1.00\xC3\x97"));
    REQUIRE(reset != nullptr);
    CHECK(*reset == wxString::FromUTF8("Volver a 1,00\xC3\x97"));

    const wxString* dash =
        catalog.find(wxString::FromUTF8("Rubber Band \xE2\x80\x94 Finer"));
    REQUIRE(dash != nullptr);
    CHECK(*dash == wxString::FromUTF8("Rubber Band: m\xC3\xA1s fino"));

    // And the same key read the wrong way finds nothing, which is what the
    // mojibake was: a miss, followed by the mangled original being displayed.
    // Spelled with an explicit Latin-1 conversion rather than with FromAscii,
    // which asserts on a non-ASCII byte in a debug build -- the very check that
    // would have caught this had the broken path gone through it. The implicit
    // conversion `_()` uses does not.
    CHECK(catalog.find(wxString("Reset to 1.00\xC3\x97", wxConvISO8859_1)) ==
          nullptr);
}

TEST_CASE("a plural message keeps both of its forms", "[wx][locale]") {
    const Loaded catalog{spanish()};

    // Keyed on the singular msgid, with the form chosen by the rule in the
    // header. This is the case the NUL-joined value exists for, and the one an
    // off-by-one in the length would break while leaving every ordinary message
    // working.
    const wxString* one = catalog.find("Add %zu Track", 1);
    REQUIRE(one != nullptr);
    CHECK(*one == wxString::FromUTF8("a\xC3\xB1"
                                     "adir %zu pista"));

    const wxString* many = catalog.find("Add %zu Track", 3);
    REQUIRE(many != nullptr);
    CHECK(*many == wxString::FromUTF8("a\xC3\xB1"
                                      "adir %zu pistas"));

    // Zero is plural in Spanish, as it is in English.
    const wxString* none = catalog.find("Add %zu Track", 0);
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
