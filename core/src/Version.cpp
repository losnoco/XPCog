#include "xpcog/core/Version.hpp"

#include <string>
#include <string_view>

namespace xpcog {

std::string_view versionBanner() {
    static const std::string banner = "XPCog " + std::string(kVersionString);
    return banner;
}

std::string_view userAgent() {
    // "+url" in the comment is the convention for a contact address rather
    // than a referer, which is how crawlers and API clients have written it
    // for long enough that log tooling recognises it.
    static const std::string agent =
        "XPCog/" + std::string(kVersionString) + " (+" + std::string(kProjectUrl) + ")";
    return agent;
}

}  // namespace xpcog
