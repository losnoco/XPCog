// The two pieces of the interface that are pure functions, and are therefore the
// two that can still be tested without one.
//
// Both replace something the Qt suite covered, and both are worth keeping for the
// same reason: they encode a format that crosses a boundary -- one into the
// settings store, one between two processes -- where a mistake is silent.

#include "OpenUrlDialog.hpp"
#include "SingleInstance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace xpcog::app;

// --- the Open URL history ------------------------------------------------

TEST_CASE("the URL history splits on newlines and drops blanks", "[wx][url]") {
    CHECK(urlHistoryFrom("").empty());
    CHECK(urlHistoryFrom("\n\n\n").empty());

    const std::vector<std::string> parsed =
        urlHistoryFrom("http://one/\nhttp://two/\n\n  http://three/  \n");
    CHECK(parsed == std::vector<std::string>{"http://one/", "http://two/",
                                             "http://three/"});
}

TEST_CASE("a repeated URL moves to the end rather than being added twice",
          "[wx][url]") {
    std::vector<std::string> history = {"http://a/", "http://b/", "http://c/"};
    history = urlHistoryWith(history, "http://b/");

    // Newest last, and exactly once. Re-opening a station should keep it to hand,
    // not fill the list with itself.
    CHECK(history ==
          std::vector<std::string>{"http://a/", "http://c/", "http://b/"});
}

TEST_CASE("the history is capped at fifteen, oldest first out", "[wx][url]") {
    std::vector<std::string> history;
    for (int i = 0; i < 20; ++i) {
        history = urlHistoryWith(history, "http://host/" + std::to_string(i));
    }

    // Cog's kMaximumURLs.
    REQUIRE(history.size() == 15);
    CHECK(history.front() == "http://host/5");
    CHECK(history.back() == "http://host/19");
}

TEST_CASE("the history round-trips through storage", "[wx][url]") {
    const std::vector<std::string> history = {"http://one/", "file:///music/a.flac",
                                              "https://two/stream?x=1"};
    CHECK(urlHistoryFrom(joinUrlHistory(history)) == history);
}

// --- the single-instance handover ---------------------------------------

TEST_CASE("the handover payload round-trips", "[wx][instance]") {
    const std::vector<std::string> arguments = {
        R"(C:\Music\Sigur Rós\Ágætis byrjun\01 Intro.flac)",
        "/home/kevin/music/track.flac",
        "https://stream.example/live",
    };

    // Non-ASCII on purpose: the payload is bytes, and a step that decoded it as
    // the active code page anywhere along the way would corrupt exactly this.
    CHECK(SingleInstance::decode(SingleInstance::encode(arguments)) == arguments);
}

TEST_CASE("an empty handover carries no arguments", "[wx][instance]") {
    // A second launch with no files is asking for the window, not for a track --
    // so this is a real case rather than a degenerate one.
    CHECK(SingleInstance::encode({}).empty());
    CHECK(SingleInstance::decode("").empty());
}

TEST_CASE("the handover identity is per user rather than per machine",
          "[wx][instance]") {
    const SingleInstance instance;
    // On Unix the lock file and the socket live in shared directories, so a bare
    // "XPCog" would mean the first user to log in owns the name and the second
    // one's player hands its files to a session it cannot see.
    CHECK(instance.name().starts_with("XPCog-"));
    CHECK(instance.name().size() > std::string{"XPCog-"}.size());

    // Explicit names are what tests use, because the default deliberately
    // collides with a running player.
    const SingleInstance named{"XPCog-test"};
    CHECK(named.name() == "XPCog-test");
}
