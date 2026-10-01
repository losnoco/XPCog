#include "Translations.hpp"

#include "catalogs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace xpcog::app {
namespace {

/// How many plural forms the catalogue has, and which one `n` selects.
///
/// Only the two shapes this project's catalogues actually carry are understood.
/// A general Plural-Forms evaluator is a small C expression parser, and writing
/// one for a build with one Spanish catalogue would be inventing a maintenance
/// burden to answer a question nobody has asked. When a language arrives that
/// needs one -- Polish and Russian are the usual first -- this is where it goes,
/// and the fallback below is what keeps the interim honest rather than wrong in
/// a way nobody notices.
enum class PluralRule {
    /// nplurals=1; plural=0;  -- Japanese, Chinese, Korean, Turkish.
    Single,
    /// nplurals=2; plural=(n != 1);  -- English, Spanish, German, and most.
    NotOne,
    /// Something else. Treated as NotOne, having said so once.
    Unknown,
};

struct Loaded {
    std::string                                            language;
    std::unordered_map<std::string_view, const CatalogEntry*> messages;
    PluralRule                                             rule = PluralRule::NotOne;
};

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

Loaded& loaded() {
    static Loaded state;
    return state;
}

/// The value of `field` in a catalogue header, or empty.
///
/// The header is one entry whose msgid is empty and whose translation is the
/// RFC-822-ish block gettext puts there. Read rather than generated so
/// cmake/CompileCatalog.cmake stays a transcriber that never interprets what it
/// copies.
std::string headerField(std::string_view header, std::string_view field) {
    std::size_t at = header.find(field);
    while (at != std::string_view::npos) {
        const bool atLineStart = at == 0 || header[at - 1] == '\n';
        if (atLineStart) {
            const std::size_t from = at + field.size();
            const std::size_t to   = header.find('\n', from);
            std::string_view  value =
                header.substr(from, to == std::string_view::npos
                                        ? std::string_view::npos
                                        : to - from);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return std::string(value);
        }
        at = header.find(field, at + 1);
    }
    return {};
}

PluralRule ruleFrom(std::string_view spec) {
    // Compared with the whitespace taken out, because a .po is hand-edited and
    // "nplurals=2;plural=(n != 1);" is the same rule spelled tighter.
    std::string tight;
    tight.reserve(spec.size());
    for (const char c : spec) {
        if (c != ' ' && c != '\t') {
            tight.push_back(c);
        }
    }
    if (tight == "nplurals=1;plural=0;") {
        return PluralRule::Single;
    }
    if (tight == "nplurals=2;plural=(n!=1);" || tight == "nplurals=2;plural=n!=1;") {
        return PluralRule::NotOne;
    }
    return PluralRule::Unknown;
}

/// Which of `forms` a count selects.
std::size_t formFor(PluralRule rule, std::size_t n) {
    switch (rule) {
        case PluralRule::Single:
            return 0;
        case PluralRule::NotOne:
        case PluralRule::Unknown:
            break;
    }
    return n == 1 ? 0 : 1;
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

}  // namespace

void installNeutralTranslations(const std::string& language) {
    Loaded& state = loaded();
    state.language.clear();
    state.messages.clear();
    state.rule = PluralRule::NotOne;

    if (language.empty()) {
        return;
    }

    for (const Catalog& catalog : catalogs()) {
        if (language != catalog.language) {
            continue;
        }

        state.language = language;
        for (const CatalogEntry& entry : catalog.entries) {
            // The header, whose msgid is empty. It carries Plural-Forms and is
            // not a message anyone looks up.
            if (entry.singular != nullptr && entry.singular[0] == '\0') {
                if (entry.forms[0] != nullptr) {
                    const std::string spec =
                        headerField(entry.forms[0], "Plural-Forms:");
                    state.rule = ruleFrom(spec);
                    if (state.rule == PluralRule::Unknown) {
                        std::fprintf(stderr,
                                     "XPCog: the %s catalogue's plural rule "
                                     "(\"%s\") is not one this build knows; "
                                     "falling back to the English rule.\n",
                                     catalog.language, spec.c_str());
                    }
                }
                continue;
            }
            if (entry.singular != nullptr) {
                state.messages.emplace(entry.singular, &entry);
            }
        }
        return;
    }
}

const std::string& currentLanguage() { return loaded().language; }

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


std::string tr(const char* msgid) {
    if (msgid == nullptr) {
        return {};
    }
    const Loaded& state = loaded();
    const auto    found = state.messages.find(std::string_view(msgid));
    if (found == state.messages.end() || found->second->forms[0] == nullptr) {
        return msgid;
    }
    return found->second->forms[0];
}

std::string trn(const char* singular, const char* plural, std::size_t n) {
    const char* fallback = (n == 1) ? singular : plural;
    if (singular == nullptr) {
        return fallback == nullptr ? std::string() : std::string(fallback);
    }

    const Loaded& state = loaded();
    const auto    found = state.messages.find(std::string_view(singular));
    if (found == state.messages.end()) {
        return fallback == nullptr ? std::string() : std::string(fallback);
    }

    // A message that is not plural in the catalogue cannot answer a plural
    // question. cmake/CompileCatalog.cmake drops a half-translated plural rather
    // than emitting one with a hole, so a hit here either has every form or is
    // not a plural at all.
    const CatalogEntry& entry = *found->second;
    if (entry.plural == nullptr) {
        return fallback == nullptr ? std::string() : std::string(fallback);
    }

    const std::size_t form = formFor(state.rule, n);
    if (form < 4 && entry.forms[form] != nullptr) {
        return entry.forms[form];
    }
    return fallback == nullptr ? std::string() : std::string(fallback);
}

namespace detail {

std::string toText(std::string value) { return value; }
std::string toText(std::string_view value) { return std::string(value); }
std::string toText(const char* value) {
    return value == nullptr ? std::string() : std::string(value);
}
std::string toText(int value) { return std::to_string(value); }
std::string toText(unsigned value) { return std::to_string(value); }
std::string toText(long value) { return std::to_string(value); }
std::string toText(unsigned long value) { return std::to_string(value); }
std::string toText(long long value) { return std::to_string(value); }
std::string toText(unsigned long long value) { return std::to_string(value); }

std::string substitute(std::string_view                format,
                       const std::vector<std::string>& args) {
    std::string out;
    out.reserve(format.size());

    std::size_t next = 0;
    for (std::size_t i = 0; i < format.size();) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            ++i;
            continue;
        }

        // How long the specifier starting at `i` is, or 0 if it is not one this
        // knows. Length-modifier first so "%zu" is not read as "%z".
        std::size_t width = 0;
        if (format.compare(i, 3, "%zu") == 0) {
            width = 3;
        } else if (format.compare(i, 2, "%s") == 0 ||
                   format.compare(i, 2, "%d") == 0 ||
                   format.compare(i, 2, "%u") == 0) {
            width = 2;
        } else if (format.compare(i, 2, "%%") == 0) {
            out.push_back('%');
            i += 2;
            continue;
        }

        if (width == 0 || next >= args.size()) {
            // Not a specifier, or one the translation added. Emitted as it
            // stands: a sentence with a stray "%s" in it still reads, and
            // dropping the message would not.
            out.push_back(format[i]);
            ++i;
            continue;
        }

        out += args[next];
        ++next;
        i += width;
    }

    return out;
}

}  // namespace detail
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

}  // namespace xpcog::app
