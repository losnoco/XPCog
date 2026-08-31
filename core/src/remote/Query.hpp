// Taking a query string apart.
//
// The tree has percentEncode() and formEncode() and no decoder for either: the
// Last.fm client only ever builds requests, so nothing until now had to read
// one. percentDecode() in Url.hpp is the piece that does exist and is used here.
//
// '+' is decoded as a space. That is not RFC 3986 -- which gives '+' no special
// meaning in a query -- but it is what every HTML form and every HTTP client
// library does, and a query string that arrives from a browser will contain
// them. The encoder next door emits %20 rather than '+' and says why; being
// strict on the way out and lenient on the way in is the right pairing.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::remote {

class Query {
public:
    /// `text` is the part after '?', still encoded.
    explicit Query(std::string_view text);

    /// The first value for `name`, or nothing. First rather than last because a
    /// repeated parameter is a client bug and the first one is the one it meant.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const;

    /// Every value for `name`, in order -- for the parameters that legitimately
    /// repeat.
    [[nodiscard]] std::vector<std::string> all(std::string_view name) const;

    [[nodiscard]] bool has(std::string_view name) const;

private:
    std::vector<std::pair<std::string, std::string>> pairs_;
};

}  // namespace xpcog::remote
