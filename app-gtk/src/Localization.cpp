#include "Localization.hpp"

#include "Glib.hpp"
#include "Translations.hpp"
#include "catalogs.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <libintl.h>

#include <cctype>
#include <clocale>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace xpcog::gtk {
namespace {

constexpr const char* kDomain = "xpcog";

/// The two-letter part of "es_CL.UTF-8".
std::string_view languageOf(std::string_view name) {
    const std::size_t cut = name.find_first_of("_.@");
    return cut == std::string_view::npos ? name : name.substr(0, cut);
}

/// A compiled-in catalogue for `code`, or null.
const app::Catalog* catalogFor(std::string_view code) {
    for (const app::Catalog& catalog : app::catalogs()) {
        if (catalog.language == code) {
            return &catalog;
        }
    }
    return nullptr;
}

/// Writes `image` to `path` unless the file already holds exactly it, so a
/// launch costs a read rather than a write once the file exists.
bool writeIfChanged(const std::filesystem::path& path, const std::string& image) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            const std::string existing((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            if (existing == image) {
                return true;
            }
        }
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    return static_cast<bool>(out);
}

/// Makes gettext honour LANGUAGE, which it does under any locale but C.
///
/// glibc ignores LANGUAGE while LC_MESSAGES is "C", and "C.UTF-8" counts: a
/// listener whose system has no locale configured -- a minimal install, a
/// container, a CI runner -- who picks Spanish in Preferences got the C++
/// strings in Spanish and every string in the .ui files in English. Any other
/// locale will do, because the language itself comes from LANGUAGE; the
/// locale only has to not be C. So LC_MESSAGES alone is moved to the first
/// installed UTF-8 locale of a short list -- the language's own, then
/// English -- and the rest of the locale is left exactly as it was.
void letGettextReadLanguage(const std::string& code) {
    const char* current = std::setlocale(LC_MESSAGES, nullptr);
    const std::string_view name = current != nullptr ? current : "C";
    if (name != "C" && name != "POSIX" && !name.starts_with("C.")) {
        return;
    }
    std::string region = code;
    for (char& c : region) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const std::string candidates[] = {code + "_" + region + ".UTF-8", "en_US.UTF-8", "en_GB.UTF-8"};
    for (const std::string& candidate : candidates) {
        if (std::setlocale(LC_MESSAGES, candidate.c_str()) != nullptr) {
            return;
        }
    }
    g_message("XPCog: no UTF-8 locale is installed beside \"%s\", so gettext ignores the "
              "chosen language and the interface files stay in English",
              std::string(name).c_str());
}

}  // namespace

std::string installTranslations(const std::string& setting) {
    // The setting names a catalogue, or "en", or nothing. Nothing means the
    // desktop's choice: GLib's list is the LANGUAGE/LC_ALL/LANG chain already
    // expanded into its fallbacks -- "es_CL.UTF-8", "es_CL", "es", "C" -- so
    // the first entry with a catalogue is the right one.
    std::string code;
    if (!setting.empty()) {
        if (catalogFor(setting) != nullptr) {
            code = setting;
        }
        // "en", or a code with no catalogue, is English: nothing to load.
    } else {
        for (const char* const* name = g_get_language_names(); name != nullptr && *name != nullptr;
             ++name) {
            const std::string_view language = languageOf(*name);
            if (catalogFor(language) != nullptr) {
                code = std::string(language);
                break;
            }
        }
    }

    app::installNeutralTranslations(code);

    if (code.empty()) {
        // English. The domain is deliberately not bound anywhere: a lookup that
        // finds no catalogue answers with the msgid, which is what is wanted.
        // LANGUAGE is pinned so a desktop in Spanish with the setting on
        // English does not get the .ui half in Spanish from a file an earlier
        // launch left behind.
        g_setenv("LANGUAGE", "C", TRUE);
        return code;
    }

    const app::Catalog* catalog = catalogFor(code);
    const std::string   image   = app::assembleCatalog(*catalog);
    const std::filesystem::path root =
        pathFromUtf8(platform::cacheDirectory()) / "locale";
    const std::filesystem::path file = root / code / "LC_MESSAGES" / (std::string(kDomain) + ".mo");

    if (!writeIfChanged(file, image)) {
        // A read-only cache directory. The C++ strings still translate; the
        // .ui strings stay in English, and this is the one place it is said.
        g_warning("XPCog: could not write %s; the interface files stay in English",
                  file.string().c_str());
        return code;
    }

    // Before the first lookup, which is before the first .ui is built. gettext
    // reads LANGUAGE for the language and the bound directory for the file,
    // and the codeset is what turns a lookup into UTF-8 whatever the locale.
    g_setenv("LANGUAGE", code.c_str(), TRUE);
    letGettextReadLanguage(code);
    bindtextdomain(kDomain, root.string().c_str());
    bind_textdomain_codeset(kDomain, "UTF-8");
    return code;
}

}  // namespace xpcog::gtk
