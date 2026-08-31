#include "Router.hpp"

#include <algorithm>

namespace xpcog::remote {
namespace {

/// Splits on '/', dropping the empty piece a leading slash produces.
std::vector<std::string_view> segments(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        if (path.front() == '/') {
            path.remove_prefix(1);
            continue;
        }
        const std::size_t end = path.find('/');
        parts.push_back(path.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        path = path.substr(end);
    }
    return parts;
}

bool isPlaceholder(std::string_view segment) {
    return segment.size() >= 2 && segment.front() == '{' && segment.back() == '}';
}

}  // namespace

Router::Router() {
    for (const Route& route : routes()) {
        if (route.path.find('{') == std::string_view::npos) {
            exact_[route.path].push_back(&route);
        } else {
            templated_.push_back(&route);
        }
    }
}

Router::Match Router::find(std::string_view method, std::string_view path) const {
    Match match;

    // A trailing slash is the same endpoint. Refusing it would be correct by the
    // letter and unhelpful in every other way.
    if (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }

    const auto exact = exact_.find(path);
    if (exact != exact_.end()) {
        for (const Route* route : exact->second) {
            if (methodName(route->method) == method) {
                match.route = route;
                return match;
            }
            match.allowed.push_back(methodName(route->method));
        }
        return match;
    }

    const std::vector<std::string_view> given = segments(path);
    for (const Route* route : templated_) {
        const std::vector<std::string_view> pattern = segments(route->path);
        if (pattern.size() != given.size()) {
            continue;
        }

        std::vector<std::pair<std::string, std::string>> captured;
        bool                                             fits = true;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (isPlaceholder(pattern[i])) {
                const std::string_view name =
                    pattern[i].substr(1, pattern[i].size() - 2);
                captured.emplace_back(std::string{name}, std::string{given[i]});
            } else if (pattern[i] != given[i]) {
                fits = false;
                break;
            }
        }
        if (!fits) {
            continue;
        }

        if (methodName(route->method) == method) {
            match.route = route;
            match.path  = std::move(captured);
            return match;
        }
        match.allowed.push_back(methodName(route->method));
    }

    return match;
}

}  // namespace xpcog::remote
