// The credits both About dialogs show. A licence list has to be right rather
// than convenient, and these are the checks that do not need a person: every
// row says what it is, under what terms and why it is here, and nothing is
// listed twice.

#include "Credits.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

using xpcog::app::Component;

namespace {

void checkRows(std::span<const Component> rows, std::set<std::string>& seen) {
    REQUIRE_FALSE(rows.empty());
    for (const Component& row : rows) {
        INFO(row.name);
        CHECK_FALSE(std::string_view(row.name).empty());
        CHECK_FALSE(std::string_view(row.licence).empty());
        CHECK_FALSE(std::string_view(row.purpose).empty());
        CHECK(seen.insert(row.name).second);
    }
}

[[nodiscard]] bool lists(std::span<const Component> rows, std::string_view name) {
    for (const Component& row : rows) {
        if (name == row.name) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("every credit names its licence and its purpose, once", "[credits]") {
    std::set<std::string> seen;
    checkRows(xpcog::app::playerComponents(), seen);
    checkRows(xpcog::app::winuiComponents(), seen);
    checkRows(xpcog::app::gtkComponents(), seen);
    checkRows(xpcog::app::codecComponents(), seen);
    checkRows(xpcog::app::dataComponents(), seen);
}

TEST_CASE("each frontend credits its own toolkit", "[credits]") {
    // The GTK About shipped 2.0.0 with no credits at all; these are the rows
    // that are easiest to lose, because they are not in the shared list.
    CHECK(lists(xpcog::app::gtkComponents(), "GTK"));
    CHECK(lists(xpcog::app::gtkComponents(), "libadwaita"));
    CHECK(lists(xpcog::app::winuiComponents(), "WinUI 3"));
    CHECK(lists(xpcog::app::winuiComponents(), "Win2D"));
    // And the remote control's server, which the old list predated.
    CHECK(lists(xpcog::app::playerComponents(), "cpp-httplib"));
}
