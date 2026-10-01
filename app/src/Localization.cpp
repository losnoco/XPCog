#include "Localization.hpp"

#include "Translations.hpp"

#include "Text.hpp"

#include <wx/arrstr.h>
#include <wx/buffer.h>
#include <wx/translation.h>
#include <wx/uilocale.h>

#include <array>
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

    // And the same catalogue again, for the lookup that has no wx to ask. See
    // Translations.hpp for why there are two and why they must not disagree.
    //
    // The language handed over is the one wx *settled on*, not the one asked
    // for. They differ in the ordinary case: with no setting, the code above
    // leaves the choice to AddCatalog, which matches the desktop's languages
    // against what the loader has -- so asking for "" here would put a Spanish
    // desktop's window half in Spanish and half in English, and only for the
    // listeners who never set the language by hand.
    installNeutralTranslations(
        toUtf8(translations->GetBestTranslation(wxString::FromAscii(kDomain))));
}

}  // namespace xpcog::app
