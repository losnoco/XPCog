#include "xpcog/core/Version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version banner is populated", "[version]") {
    const auto banner = xpcog::versionBanner();
    CHECK_FALSE(banner.empty());
    CHECK(banner.starts_with("XPCog "));
    CHECK(xpcog::kVersionMajor >= 0);
    CHECK_FALSE(xpcog::kVersionString.empty());
}
