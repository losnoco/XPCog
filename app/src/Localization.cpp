#include "Localization.hpp"

#include "Text.hpp"

#include <wx/arrstr.h>
#include <wx/buffer.h>
#include <wx/translation.h>
#include <wx/uilocale.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace xpcog::app {
namespace {

/// The catalogue's domain. One domain, because there is one application.
constexpr const char* kDomain = "xpcog";

/// What each language calls itself.
///
/// Written here rather than read out of the .po, and the alternative is worth
/// saying no to explicitly: a translator can put anything in a `Language-Team`
/// header, and a picker built from that would show whatever the last person to
/// edit the file happened to type. wx can name a language too, but it names it
/// in the *current* interface language -- so the Spanish entry would read
/// "Spanish" to an English speaker looking for "Espanol", which is exactly the
/// listener this row exists for.
///
/// A code with no row here falls back to the code itself, which is ugly and
/// visible, rather than to a blank row, which is not.
struct Endonym {
    const char* code;
    const char* name;
};

constexpr std::array kEndonyms = {
    Endonym{"en", "English"},
    Endonym{"es", "Espa\xC3\xB1ol"},
};

[[nodiscard]] std::string endonymFor(std::string_view code) {
    for (const Endonym& entry : kEndonyms) {
        if (code == entry.code) {
            return entry.name;
        }
    }
    return std::string{code};
}

/// gettext's `.mo` is little-endian when its magic is written this way round,
/// on every machine -- the format carries the byte order in the magic rather
/// than following the host's.
void appendLittleEndian(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24) & 0xFFU));
}

/// Serves the compiled-in catalogues to wxTranslations.
///
/// The loader interface is the whole extension point wx offers here: the two
/// implementations it ships read a directory of .mo files and a Windows resource
/// section, and neither is what a catalogue living in a `const char*` table
/// wants. Answering `GetAvailableTranslations` honestly matters as much as
/// `LoadCatalog` does -- it is what "follow the system" is matched against, so a
/// loader that under-reports simply never gets asked for the language it holds.
class EmbeddedCatalogs : public wxTranslationsLoader {
public:
    wxMsgCatalog* LoadCatalog(const wxString& domain, const wxString& language) override {
        if (domain != wxString::FromAscii(kDomain)) {
            return nullptr;
        }
        for (const Catalog& catalog : catalogs()) {
            if (language != wxString::FromAscii(catalog.language)) {
                continue;
            }
            const std::string image = assembleCatalog(catalog);
            // An owning buffer rather than wxScopedCharBuffer::CreateNonOwned.
            // wxMsgCatalog copies every message out into a hash map as it parses
            // and keeps none of the bytes, so a non-owned view over this local
            // would in fact survive -- but that is a property of wx's parser
            // rather than of its contract, and a catalogue that reads as garbage
            // the day it stops holding is not a failure anyone would trace back
            // to here.
            wxCharBuffer bytes(image.size());
            std::memcpy(bytes.data(), image.data(), image.size());
            return wxMsgCatalog::CreateFromData(bytes, domain);
        }
        return nullptr;
    }

    wxArrayString GetAvailableTranslations(const wxString& domain) const override {
        wxArrayString languages;
        if (domain != wxString::FromAscii(kDomain)) {
            return languages;
        }
        for (const Catalog& catalog : catalogs()) {
            languages.Add(wxString::FromAscii(catalog.language));
        }
        return languages;
    }
};

}  // namespace

std::vector<LanguageOption> availableLanguages() {
    std::vector<LanguageOption> options;
    // Empty rather than a code, because "follow the system" is not a language:
    // storing `en` for a listener whose desktop is Spanish would pin them to
    // English for good the first time they opened this row to look at it.
    options.push_back(LanguageOption{"", ""});
    options.push_back(LanguageOption{"en", endonymFor("en")});
    for (const Catalog& catalog : catalogs()) {
        options.push_back(
            LanguageOption{catalog.language, endonymFor(catalog.language)});
    }
    return options;
}

std::string assembleCatalog(const Catalog& catalog) {
    // Key and value in gettext's own shape: a context is joined to the msgid
    // with EOT, and the plural forms of either side are joined with NUL. Both
    // are what the format says and what wx's parser splits on again -- doing it
    // here is what lets the generated table stay a list of plain C strings.
    struct Message {
        std::string key;
        std::string value;
    };

    std::vector<Message> messages;
    messages.reserve(catalog.entries.size());

    for (const CatalogEntry& entry : catalog.entries) {
        Message message;
        if (entry.context != nullptr) {
            message.key = entry.context;
            message.key.push_back('\x04');
        }
        message.key += entry.singular;
        if (entry.plural != nullptr) {
            message.key.push_back('\0');
            message.key += entry.plural;
        }

        bool first = true;
        for (const char* form : entry.forms) {
            if (form == nullptr) {
                break;
            }
            if (!first) {
                message.value.push_back('\0');
            }
            message.value += form;
            first = false;
        }
        messages.push_back(std::move(message));
    }

    // Sorted by key, which the format requires so that a reader may binary
    // search. wx builds a hash map instead and would not notice, but a .mo that
    // is only readable by the one parser that happens to be lenient is not the
    // format it claims to be -- and this image is exactly what would be written
    // out if these ever needed handing to msgunfmt.
    std::sort(messages.begin(), messages.end(),
              [](const Message& left, const Message& right) {
                  return left.key < right.key;
              });

    const auto count = static_cast<std::uint32_t>(messages.size());

    // The layout, in order: a 28-byte header, the originals' index, the
    // translations' index, then the strings themselves. The hash table is
    // optional and omitted -- a size of zero is how the format says so.
    constexpr std::uint32_t kHeaderSize = 28;
    const std::uint32_t     originals   = kHeaderSize;
    const std::uint32_t     translated  = originals + (count * 8);
    const std::uint32_t     strings     = translated + (count * 8);

    std::string image;
    appendLittleEndian(image, 0x950412DEU);  // magic
    appendLittleEndian(image, 0);            // revision
    appendLittleEndian(image, count);
    appendLittleEndian(image, originals);
    appendLittleEndian(image, translated);
    appendLittleEndian(image, 0);  // hash table size
    appendLittleEndian(image, 0);  // hash table offset

    // Every string is stored NUL-terminated and its length is reported *without*
    // that NUL -- which is what makes the embedded NULs above work: the reported
    // length is what a parser slices on, and the terminator is only there so a
    // C string function reaching the buffer finds an end.
    std::uint32_t at = strings;
    for (const Message& message : messages) {
        appendLittleEndian(image, static_cast<std::uint32_t>(message.key.size()));
        appendLittleEndian(image, at);
        at += static_cast<std::uint32_t>(message.key.size()) + 1;
    }
    for (const Message& message : messages) {
        appendLittleEndian(image, static_cast<std::uint32_t>(message.value.size()));
        appendLittleEndian(image, at);
        at += static_cast<std::uint32_t>(message.value.size()) + 1;
    }

    for (const Message& message : messages) {
        image += message.key;
        image.push_back('\0');
    }
    for (const Message& message : messages) {
        image += message.value;
        image.push_back('\0');
    }

    return image;
}

void installTranslations(const std::string& language) {
    // A code this build has no catalogue for is treated as "follow the system"
    // rather than honoured: a settings file that has travelled from a build with
    // more languages in it should fall back to the desktop's choice, not to a
    // language nothing can supply.
    bool known = language.empty() || language == "en";
    for (const Catalog& catalog : catalogs()) {
        if (language == catalog.language) {
            known = true;
        }
    }

    auto* translations = new wxTranslations;
    translations->SetLoader(new EmbeddedCatalogs);
    // Takes ownership, and replaces whatever was there -- including the instance
    // a wxLocale would have installed, which is why nothing here constructs one.
    wxTranslations::Set(translations);

    if (known && !language.empty()) {
        translations->SetLanguage(toWx(language));

        // And the formatting with it, so a Spanish interface does not write
        // dates and decimal points the American way. Separate from the
        // catalogue on purpose: wxUILocale is about how numbers and dates are
        // *spelled*, wxTranslations about which strings are shown, and asking
        // for one has never implied the other since wxLocale stopped being the
        // way to do either. A name the system does not have simply fails, and
        // the interface is still translated.
        static_cast<void>(wxUILocale::UseLocaleName(toWx(language)));
    }
    // Nothing to do for the system case: with no language set, AddCatalog picks
    // the best match between what the desktop asks for and what the loader says
    // it has -- which is the whole reason GetAvailableTranslations has to be
    // answered properly above.

    // False means no catalogue was loaded, which is the ordinary case for
    // English and for a desktop in a language nobody has translated yet. Not
    // worth reporting: the interface is in the language the msgids are written
    // in, which is a working player rather than a degraded one.
    static_cast<void>(translations->AddCatalog(wxString::FromAscii(kDomain)));
}

}  // namespace xpcog::app
