// The command tables' toolkit-free half: the action names.

#include "Commands.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace xpcog::app;

TEST_CASE("every command has an action name, and the names are stable", "[uicore][commands]") {
    // Every id in the enum, which is what lets a command added later fail here
    // rather than arrive with no way to be invoked from a name-based surface.
    std::set<std::pair<std::string, std::string>> seen;
    for (const CommandId id : allCommands()) {
        const CommandAction action = commandAction(id);
        REQUIRE(action.name != nullptr);
        CHECK_FALSE(std::string(action.name).empty());
        for (const char c : std::string(action.name)) {
            // kebab-case: what a .ui file and a shortcut editor both accept.
            CHECK(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'));
        }
        // A (name, target) pair is unique: two commands on one pair would be
        // one action doing two things.
        const auto pair = std::make_pair(std::string(action.name),
                                         std::string(action.target ? action.target : ""));
        CHECK(seen.insert(pair).second);
    }
    // Every enumerator, once. The count is the enum's, and moves with it.
    CHECK(allCommands().size() == static_cast<std::size_t>(seen.size()));
    CHECK(allCommands().size() == 50);
}

TEST_CASE("the radio groups are one action each, by target", "[uicore][commands]") {
    CHECK(std::string(commandAction(OrderRepeatNone).name) == "repeat");
    CHECK(std::string(commandAction(OrderRepeatAll).name) == "repeat");
    CHECK(std::string(commandAction(OrderRepeatAll).target) == "all");
    CHECK(std::string(commandAction(OrderShuffleAlbums).name) == "shuffle");
    CHECK(std::string(commandAction(ViewFollowPlayback).name) == "panels-follow");
    CHECK(commandAction(PlaybackPlayPause).target == nullptr);
}

TEST_CASE("every menu row names a command the action table knows", "[uicore][commands]") {
    for (const MenuItem& item : menuLayout()) {
        CHECK_FALSE(std::string(commandAction(item.id).name).empty());
    }
    for (const MenuItem& item : playlistMenuLayout()) {
        CHECK_FALSE(std::string(commandAction(item.id).name).empty());
    }
}
