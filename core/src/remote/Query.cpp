#include "Query.hpp"

#include "xpcog/core/Url.hpp"

#include <algorithm>
#include <utility>

namespace xpcog::remote {
namespace {

/// percentDecode() with '+' meaning a space, which is what a form sends.
std::string decode(std::string_view text) {
    std::string plussed{text};
    std::replace(plussed.begin(), plussed.end(), '+', ' ');
    return percentDecode(plussed);
}

}  // namespace

Query::Query(std::string_view text) {
    while (!text.empty()) {
        const std::size_t   end  = text.find('&');
        const std::string_view one = text.substr(0, end);
        text = (end == std::string_view::npos) ? std::string_view{}
                                               : text.substr(end + 1);
        if (one.empty()) {
            continue;
        }
        const std::size_t equals = one.find('=');
        if (equals == std::string_view::npos) {
            // A bare `?verbose` is a name with an empty value rather than
            // nothing at all, so has() can answer about it.
            pairs_.emplace_back(decode(one), std::string{});
        } else {
            pairs_.emplace_back(decode(one.substr(0, equals)),
                                decode(one.substr(equals + 1)));
        }
    }
}

std::optional<std::string> Query::get(std::string_view name) const {
    for (const auto& [key, value] : pairs_) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::vector<std::string> Query::all(std::string_view name) const {
    std::vector<std::string> values;
    for (const auto& [key, value] : pairs_) {
        if (key == name) {
            values.push_back(value);
        }
    }
    return values;
}

bool Query::has(std::string_view name) const { return get(name).has_value(); }

}  // namespace xpcog::remote
