// Percent-encoding, in its own translation unit because it is needed whether or
// not this build has libcurl: the Last.fm signature layer is compiled either
// way, and so are the tests that pin the encoding.
//
// Hand-written rather than `curl_easy_escape`, for that reason and one more:
// curl's escape needs a handle, so the encoding used to *sign* a request would
// otherwise depend on a transport that a test does not have.

#include "xpcog/core/net/HttpClient.hpp"

namespace xpcog {
namespace {

/// RFC 3986's unreserved set. Everything else is escaped, including the
/// characters a form encoder is often lax about -- `*`, `!`, `'`, `(`, `)` --
/// because Last.fm signs the raw value and any of those surviving unescaped
/// would arrive as a different string than the one that was signed.
[[nodiscard]] constexpr bool isUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

}  // namespace

std::string percentEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (isUnreserved(c)) {
            encoded.push_back(raw);
        } else {
            // %20 for a space, not '+'. Both decode in a form body, only one
            // decodes in a query string, and this encoder is used for both.
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string formEncode(const HttpParams& params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += percentEncode(key);
        body.push_back('=');
        body += percentEncode(value);
    }
    return body;
}

}  // namespace xpcog
