#include "xpcog/core/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <system_error>

namespace xpcog {
namespace {

[[nodiscard]] char lowerAscii(char c) noexcept {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool isSchemeStart(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

[[nodiscard]] bool isSchemeChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' ||
           c == '.';
}

/// Characters left unescaped in a path segment. Deliberately conservative: this is
/// only used for building file:// URLs, so over-escaping is harmless while
/// under-escaping would break round-tripping.
[[nodiscard]] bool isUnreservedPathChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
           c == '.' || c == '~' || c == '/';
}

[[nodiscard]] int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string percentEncodePath(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(text.size());

    for (const char c : text) {
        if (isUnreservedPathChar(c)) {
            out.push_back(c);
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

std::optional<Url> Url::parse(std::string_view text) {
    if (text.empty() || !isSchemeStart(text.front())) {
        return std::nullopt;
    }

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }

    const std::string_view scheme = text.substr(0, colon);
    if (!std::all_of(scheme.begin(), scheme.end(), isSchemeChar)) {
        return std::nullopt;
    }

    // A single-letter "scheme" is almost always a Windows drive letter
    // (C:\music\a.flac), not a URL. Reject so callers use fromLocalPath().
    if (scheme.size() < 2) {
        return std::nullopt;
    }

    Url url;
    url.scheme_.reserve(scheme.size());
    std::transform(scheme.begin(), scheme.end(), std::back_inserter(url.scheme_),
                   lowerAscii);

    std::string_view rest = text.substr(colon + 1);
    if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
        url.fragment_ = rest.substr(hash + 1);
        rest          = rest.substr(0, hash);
    }
    url.body_ = rest;

    return url;
}

Url Url::fromLocalPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    absolute = absolute.lexically_normal();

    // Always use forward slashes in the URL, including on Windows.
    std::string generic = absolute.generic_string();

    Url url;
    url.scheme_ = "file";
    // "//" is the empty authority; an absolute POSIX path already starts with '/',
    // whereas a Windows path starts with a drive letter and needs one added.
    url.body_ = generic.starts_with('/') ? "//" + percentEncodePath(generic)
                                         : "///" + percentEncodePath(generic);
    return url;
}

std::string Url::extension() const {
    const std::size_t slash = body_.find_last_of('/');
    const std::size_t start = (slash == std::string::npos) ? 0 : slash + 1;

    const std::size_t dot = body_.find_last_of('.');
    if (dot == std::string::npos || dot < start || dot + 1 >= body_.size()) {
        return {};
    }

    std::string ext = percentDecode(std::string_view{body_}.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), lowerAscii);
    return ext;
}

std::optional<std::filesystem::path> Url::localPath() const {
    if (scheme_ != "file") {
        return std::nullopt;
    }

    std::string_view path{body_};
    if (path.starts_with("//")) {
        path.remove_prefix(2);
        // Skip an authority component if one is present (file://host/path).
        if (!path.starts_with('/')) {
            if (const std::size_t slash = path.find('/');
                slash != std::string_view::npos) {
                path.remove_prefix(slash);
            }
        }
    }

    std::string decoded = percentDecode(path);
    if (decoded.empty()) {
        return std::nullopt;
    }

#ifdef _WIN32
    // "/C:/music" -> "C:/music"
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
#endif

    return std::filesystem::path{decoded};
}

std::string Url::toString() const {
    if (scheme_.empty()) {
        return {};
    }
    std::string out = scheme_ + ':' + body_;
    if (!fragment_.empty()) {
        out += '#';
        out += fragment_;
    }
    return out;
}

Url Url::withFragment(std::string_view fragment) const {
    Url copy      = *this;
    copy.fragment_ = std::string{fragment};
    return copy;
}

Url Url::withoutFragment() const {
    Url copy = *this;
    copy.fragment_.clear();
    return copy;
}

}  // namespace xpcog
