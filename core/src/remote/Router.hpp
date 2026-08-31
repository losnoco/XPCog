// Matching a request to a route.
//
// Two tables rather than one loop, and the split is what makes 405 answerable.
// A path that matches nothing is 404; a path that matches under a *different*
// method is 405 with an Allow header saying which -- and the router knows the
// difference only because it looks the path up before it looks the method up.
// A single pass keyed on both would report every wrong-method request as a
// missing endpoint, which sends the reader looking for a typo in the URL.

#pragma once

#include "Routes.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xpcog::remote {

class Router {
public:
    Router();

    struct Match {
        const Route* route = nullptr;
        /// Filled in from the `{name}` segments when a route matched.
        std::vector<std::pair<std::string, std::string>> path;
        /// The methods this path does accept, for the Allow header on a 405.
        std::vector<std::string_view> allowed;
    };

    /// `route` null with `allowed` non-empty means 405; both empty means 404.
    [[nodiscard]] Match find(std::string_view method, std::string_view path) const;

private:
    /// Patterns with no `{name}` in them, which is most of them.
    std::unordered_map<std::string_view, std::vector<const Route*>> exact_;
    /// The rest, matched segment by segment. A short list, so a linear walk is
    /// the whole of what it needs.
    std::vector<const Route*> templated_;
};

}  // namespace xpcog::remote
