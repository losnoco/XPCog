#include "xpcog/core/Version.hpp"

#include <string>

namespace xpcog {

std::string_view versionBanner() {
    static const std::string banner = "XPCog " + std::string(kVersionString);
    return banner;
}

}  // namespace xpcog
