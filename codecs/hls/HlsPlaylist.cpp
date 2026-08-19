#include "HlsPlaylist.hpp"

#include "../common/PlaylistText.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace xpcog::codecs {
namespace {

[[nodiscard]] bool isSchemeStart(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

[[nodiscard]] bool isSchemeChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' ||
           c == '.';
}

/// True when `reference` begins with its own scheme. Two characters minimum, for
/// the reason Url::parse has the same rule: `c:/music` is a drive letter.
[[nodiscard]] bool hasScheme(std::string_view reference) {
    if (reference.empty() || !isSchemeStart(reference.front())) {
        return false;
    }
    const std::size_t colon = reference.find(':');
    if (colon == std::string_view::npos || colon < 2) {
        return false;
    }
    const std::string_view scheme = reference.substr(0, colon);
    return std::all_of(scheme.begin(), scheme.end(), isSchemeChar);
}

/// RFC 3986 section 5.2.4. Without this a manifest that names `../audio/0.aac`
/// resolves to a path containing a literal "..", which some servers 404 and
/// others answer with the wrong file.
[[nodiscard]] std::string removeDotSegments(std::string_view path) {
    std::vector<std::string_view> output;
    const bool trailingSlash = !path.empty() && path.back() == '/';

    std::size_t position = 0;
    while (position < path.size()) {
        const std::size_t      slash   = path.find('/', position);
        const std::size_t      stop    = (slash == std::string_view::npos) ? path.size() : slash;
        const std::string_view segment = path.substr(position, stop - position);

        if (segment == "..") {
            if (!output.empty()) {
                output.pop_back();
            }
        } else if (segment != "." && !segment.empty()) {
            output.push_back(segment);
        }

        position = stop + 1;
    }

    std::string result;
    for (const std::string_view segment : output) {
        result += '/';
        result += segment;
    }
    if (trailingSlash || result.empty()) {
        result += '/';
    }
    return result;
}

/// Splits `body` -- a URL with its scheme already removed -- into the authority
/// ("//host:port", empty when there is none) and everything after it.
struct SplitBody {
    std::string_view authority;
    std::string_view rest;
};

[[nodiscard]] SplitBody splitAuthority(std::string_view body) {
    if (!body.starts_with("//")) {
        return {{}, body};
    }
    const std::size_t slash = body.find('/', 2);
    if (slash == std::string_view::npos) {
        return {body, {}};
    }
    return {body.substr(0, slash), body.substr(slash)};
}

[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

/// `0xA1B2...` into bytes. An odd digit count is left-padded rather than
/// rejected: a publisher that writes an IV short is describing a real IV.
[[nodiscard]] std::vector<std::byte> bytesFromHex(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) {
        hex.remove_prefix(2);
    }

    std::string digits;
    if (hex.size() % 2 == 1) {
        digits += '0';
    }
    digits += hex;

    std::vector<std::byte> out;
    out.reserve(digits.size() / 2);
    for (std::size_t i = 0; i + 2 <= digits.size(); i += 2) {
        int value = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char c = digits[i + j];
            int        digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                return {};
            }
            value = (value << 4) | digit;
        }
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/// The tag name and its value: `#EXT-X-KEY:METHOD=NONE` splits at the first
/// colon, and a tag with no value (`#EXT-X-ENDLIST`) keeps an empty one.
struct TagLine {
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] TagLine splitTag(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return {line, {}};
    }
    return {line.substr(0, colon), line.substr(colon + 1)};
}

[[nodiscard]] std::int64_t integerValue(std::string_view text) {
    return std::strtoll(std::string{text}.c_str(), nullptr, 10);
}

[[nodiscard]] double doubleValue(std::string_view text) {
    return std::strtod(std::string{text}.c_str(), nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// URI resolution
// ---------------------------------------------------------------------------

std::optional<Url> resolveHlsUri(std::string_view reference, const Url& base) {
    while (!reference.empty() &&
           std::isspace(static_cast<unsigned char>(reference.front())) != 0) {
        reference.remove_prefix(1);
    }
    while (!reference.empty() &&
           std::isspace(static_cast<unsigned char>(reference.back())) != 0) {
        reference.remove_suffix(1);
    }
    if (reference.empty()) {
        return std::nullopt;
    }

    if (hasScheme(reference)) {
        return Url::parse(reference);
    }
    if (base.empty()) {
        return std::nullopt;
    }

    const std::string scheme{base.scheme()};

    // Url exposes no accessor for the scheme-specific part, so recover it from
    // the canonical serialisation, which is `scheme:body` exactly.
    const std::string baseText = base.withoutFragment().toString();
    const std::string_view baseBody =
        std::string_view{baseText}.substr(scheme.size() + 1);
    const auto [authority, afterAuthority] = splitAuthority(baseBody);

    std::string_view basePath = afterAuthority;
    if (const std::size_t query = basePath.find('?'); query != std::string_view::npos) {
        basePath = basePath.substr(0, query);
    }

    // A protocol-relative reference replaces the authority and everything after.
    if (reference.starts_with("//")) {
        return Url::parse(scheme + ':' + std::string{reference});
    }

    // Split the reference's own query and fragment off before touching its path:
    // a `?` or `#` inside is not part of the path and must not be dot-normalised.
    std::string_view referencePath = reference;
    std::string_view referenceTail;
    if (const std::size_t mark = referencePath.find_first_of("?#");
        mark != std::string_view::npos) {
        referenceTail = referencePath.substr(mark);
        referencePath = referencePath.substr(0, mark);
    }

    std::string path;
    if (referencePath.empty()) {
        // A bare query or fragment keeps the base's path.
        path = std::string{basePath};
    } else if (referencePath.starts_with('/')) {
        path = removeDotSegments(referencePath);
    } else {
        const std::size_t lastSlash = basePath.find_last_of('/');
        std::string       merged =
            (lastSlash == std::string_view::npos)
                      ? std::string{"/"}
                      : std::string{basePath.substr(0, lastSlash + 1)};
        merged += referencePath;
        path = removeDotSegments(merged);
    }

    return Url::parse(scheme + ':' + std::string{authority} + path +
                      std::string{referenceTail});
}

// ---------------------------------------------------------------------------
// Attribute lists
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> parseAttributeList(
    std::string_view attributes) {
    std::vector<std::pair<std::string, std::string>> out;

    std::size_t position = 0;
    while (position < attributes.size()) {
        while (position < attributes.size() &&
               (attributes[position] == ' ' || attributes[position] == '\t')) {
            ++position;
        }
        if (position >= attributes.size()) {
            break;
        }

        const std::size_t keyStart = position;
        while (position < attributes.size() && attributes[position] != '=' &&
               attributes[position] != ',') {
            ++position;
        }
        std::string_view key = attributes.substr(keyStart, position - keyStart);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.remove_suffix(1);
        }

        std::string value;
        if (position < attributes.size() && attributes[position] == '=') {
            ++position;
            if (position < attributes.size() && attributes[position] == '"') {
                // A quoted value may contain commas -- CODECS="mp4a.40.2,avc1"
                // is one attribute, and splitting the list on every comma is the
                // classic way to read it as two. RFC 8216 defines no escape, so
                // the first closing quote ends the value.
                ++position;
                const std::size_t valueStart = position;
                while (position < attributes.size() && attributes[position] != '"') {
                    ++position;
                }
                value = std::string{attributes.substr(valueStart, position - valueStart)};
                if (position < attributes.size()) {
                    ++position;
                }
            } else {
                const std::size_t valueStart = position;
                while (position < attributes.size() && attributes[position] != ',') {
                    ++position;
                }
                std::string_view raw = attributes.substr(valueStart, position - valueStart);
                while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
                    raw.remove_suffix(1);
                }
                value = std::string{raw};
            }
        }

        if (!key.empty()) {
            out.emplace_back(std::string{key}, std::move(value));
        }

        if (position < attributes.size() && attributes[position] == ',') {
            ++position;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------

double HlsPlaylist::totalDuration() const {
    if (isLive) {
        return 0.0;
    }
    double total = 0.0;
    for (const HlsSegment& segment : segments) {
        total += segment.duration;
    }
    return total;
}

const HlsVariant* HlsPlaylist::bestVariant() const {
    const HlsVariant* best = nullptr;
    for (const HlsVariant& variant : variants) {
        if (variant.url.empty()) {
            continue;
        }
        if (best == nullptr || variant.bandwidth > best->bandwidth) {
            best = &variant;
        }
    }
    return best;
}

bool looksLikeHlsManifest(std::string_view text) {
    if (text.find("#EXTM3U") == std::string_view::npos) {
        return false;
    }
    return text.find("#EXT-X-TARGETDURATION") != std::string_view::npos ||
           text.find("#EXT-X-STREAM-INF") != std::string_view::npos;
}

std::optional<HlsPlaylist> parseHlsPlaylist(std::string_view text, const Url& base) {
    if (!looksLikeHlsManifest(text)) {
        return std::nullopt;
    }

    HlsPlaylist playlist;
    playlist.url = base;

    // Forward-applied state. EXT-X-KEY and EXT-X-MAP govern every segment after
    // them until another tag of the same kind supersedes them, so they are held
    // here rather than attached to whichever segment happens to follow.
    Url                    keyUrl;
    std::string            keyMethod;
    std::vector<std::byte> keyIv;
    bool                   keyEncrypted = false;
    Url                    mapUrl;
    bool                   pendingDiscontinuity = false;

    std::optional<HlsSegment> pendingSegment;
    std::optional<HlsVariant> pendingVariant;

    bool sawHeader = false;

    for (const std::string& line : splitLines(std::string{text})) {
        if (!sawHeader) {
            // RFC 8216 section 4.3.1.1: the first line must be #EXTM3U.
            if (line != "#EXTM3U") {
                return std::nullopt;
            }
            sawHeader = true;
            continue;
        }

        // A line that is not a tag terminates whichever declaration is pending.
        if (!line.starts_with('#')) {
            const std::optional<Url> url = resolveHlsUri(line, base);
            if (!url) {
                continue;
            }

            if (pendingVariant) {
                pendingVariant->url = *url;
                playlist.variants.push_back(std::move(*pendingVariant));
                pendingVariant.reset();
                playlist.isMaster = true;
            } else if (pendingSegment) {
                pendingSegment->url = *url;
                pendingSegment->sequenceNumber =
                    playlist.mediaSequence +
                    static_cast<std::int64_t>(playlist.segments.size());
                pendingSegment->discontinuitySequence = playlist.discontinuitySequence;
                pendingSegment->mapUrl                = mapUrl;
                if (keyEncrypted) {
                    pendingSegment->encrypted        = true;
                    pendingSegment->encryptionMethod = keyMethod;
                    pendingSegment->encryptionKeyUrl = keyUrl;
                    pendingSegment->iv               = keyIv;
                }
                if (pendingDiscontinuity) {
                    pendingSegment->discontinuity = true;
                    ++playlist.discontinuitySequence;
                    pendingDiscontinuity = false;
                }
                playlist.segments.push_back(std::move(*pendingSegment));
                pendingSegment.reset();
            }
            // A URI with no preceding EXTINF or EXT-X-STREAM-INF is not
            // addressable; dropping it is what Cog does.
            continue;
        }

        const auto [tag, value] = splitTag(std::string_view{line});

        if (tag == "#EXT-X-VERSION") {
            playlist.version = static_cast<int>(integerValue(value));
        } else if (tag == "#EXT-X-TARGETDURATION") {
            playlist.targetDuration = static_cast<int>(integerValue(value));
        } else if (tag == "#EXT-X-MEDIA-SEQUENCE") {
            playlist.mediaSequence = integerValue(value);
        } else if (tag == "#EXT-X-DISCONTINUITY-SEQUENCE") {
            playlist.discontinuitySequence = integerValue(value);
        } else if (tag == "#EXT-X-PLAYLIST-TYPE") {
            if (equalsIgnoringCase(value, "VOD")) {
                playlist.type   = HlsPlaylistType::Vod;
                playlist.isLive = false;
            } else if (equalsIgnoringCase(value, "EVENT")) {
                // An EVENT playlist only ever appends, but it does append, so it
                // is still live until an ENDLIST arrives.
                playlist.type = HlsPlaylistType::Event;
            }
        } else if (tag == "#EXT-X-ENDLIST") {
            playlist.hasEndList = true;
            playlist.isLive     = false;
        } else if (tag == "#EXTINF") {
            HlsSegment segment;
            const std::size_t comma = value.find(',');
            segment.duration =
                doubleValue(comma == std::string_view::npos ? value
                                                            : value.substr(0, comma));
            if (comma != std::string_view::npos) {
                segment.title = std::string{value.substr(comma + 1)};
            }
            pendingSegment = std::move(segment);
        } else if (tag == "#EXT-X-DISCONTINUITY") {
            pendingDiscontinuity = true;
        } else if (tag == "#EXT-X-KEY") {
            const auto attributes = parseAttributeList(value);
            std::string method;
            std::string uri;
            std::string iv;
            for (const auto& [key, attributeValue] : attributes) {
                if (key == "METHOD") {
                    method = attributeValue;
                } else if (key == "URI") {
                    uri = attributeValue;
                } else if (key == "IV") {
                    iv = attributeValue;
                }
            }

            if (method.empty() || equalsIgnoringCase(method, "NONE")) {
                keyEncrypted = false;
                keyMethod.clear();
                keyUrl = Url{};
                keyIv.clear();
            } else {
                keyEncrypted = true;
                keyMethod    = method;
                keyUrl       = Url{};
                if (!uri.empty()) {
                    if (const auto resolved = resolveHlsUri(uri, base)) {
                        keyUrl = *resolved;
                    }
                }
                keyIv = iv.empty() ? std::vector<std::byte>{} : bytesFromHex(iv);
            }
        } else if (tag == "#EXT-X-MAP") {
            mapUrl = Url{};
            for (const auto& [key, attributeValue] : parseAttributeList(value)) {
                if (key == "URI" && !attributeValue.empty()) {
                    if (const auto resolved = resolveHlsUri(attributeValue, base)) {
                        mapUrl = *resolved;
                    }
                }
            }
        } else if (tag == "#EXT-X-STREAM-INF") {
            HlsVariant variant;
            for (const auto& [key, attributeValue] : parseAttributeList(value)) {
                if (key == "BANDWIDTH") {
                    variant.bandwidth = integerValue(attributeValue);
                } else if (key == "AVERAGE-BANDWIDTH") {
                    variant.averageBandwidth = integerValue(attributeValue);
                } else if (key == "CODECS") {
                    variant.codecs = attributeValue;
                } else if (key == "RESOLUTION") {
                    variant.resolution = attributeValue;
                }
            }
            pendingVariant = std::move(variant);
        }
        // Everything else -- EXT-X-MEDIA, EXT-X-BYTERANGE, EXT-X-PROGRAM-DATE-TIME,
        // plain comments -- carries nothing this player acts on.
    }

    if (playlist.segments.empty() && playlist.variants.empty()) {
        return std::nullopt;
    }
    return playlist;
}

}  // namespace xpcog::codecs
