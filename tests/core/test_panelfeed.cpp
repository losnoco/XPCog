// The queue between a synthesiser's front panel and whatever draws it.
//
// No emulator here and no ROMs: the frames are opaque bytes, so everything this
// has to get right can be tested with three of them. What it has to get right
// is the part that is invisible when wrong -- a panel that lags, or that shows
// the wrong track's display across a gapless seam.
//
// Every case clears first. The feed is a process-wide singleton, deliberately
// (see PanelFeed.hpp), and Catch2 runs cases in a random order, so a case that
// did not clear would sometimes see the previous one's frames and sometimes
// not.

#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/PanelFeed.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

[[nodiscard]] Url track(const char* name) {
    return Url::fromLocalPath(std::filesystem::path{"/music"} / name);
}

/// A frame's payload only has to be distinguishable, so one byte is plenty.
[[nodiscard]] std::vector<std::byte> blob(unsigned char value) {
    return {static_cast<std::byte>(value)};
}

[[nodiscard]] unsigned char first(const PanelFrame& frame) {
    return static_cast<unsigned char>(frame.state.front());
}

}  // namespace

TEST_CASE("frames come back in order, and only once they are due", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 0.5, blob(1));
    feed.post(one, 1.5, blob(2));
    feed.post(one, 2.5, blob(3));

    // A display asks "what had happened by now", so the boundary is inclusive
    // and everything past it stays queued.
    auto due = feed.take(1.5);
    REQUIRE(due.size() == 2);
    CHECK(first(due[0]) == 1);
    CHECK(first(due[1]) == 2);

    // Drained, not copied: a panel state is shown once.
    CHECK(feed.take(1.5).empty());

    due = feed.take(10.0);
    REQUIRE(due.size() == 1);
    CHECK(first(due[0]) == 3);
}

TEST_CASE("a gapless seam does not show the wrong track's panel", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // The engine opens the next track's decoder "while its audio is still
    // playing out", so both are producing at once and both count from zero
    // within themselves. Without the keying, the second track's frames would
    // be drained against the first track's position and the panel would run
    // ahead into music nobody has heard yet.
    const Url playing = track("playing.mid");
    const Url next    = track("next.mid");
    feed.setAudibleTrack(playing);

    feed.post(playing, 1.0, blob(10));
    feed.post(next, 0.1, blob(20));
    feed.post(next, 0.2, blob(21));

    const auto due = feed.take(5.0);
    REQUIRE(due.size() == 1);
    CHECK(first(due[0]) == 10);

    // And when the seam is reached, the one that was waiting is what plays.
    feed.setAudibleTrack(next);
    const auto after = feed.take(5.0);
    REQUIRE(after.size() == 2);
    CHECK(first(after[0]) == 20);
    CHECK(first(after[1]) == 21);
}

TEST_CASE("nothing is drained before the speaker has reached a track",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    feed.post(track("early.mid"), 1.0, blob(1));
    CHECK(feed.take(60.0).empty());
}

TEST_CASE("a seek drops the panel states it skipped past", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));
    feed.post(one, 2.0, blob(2));

    // Every queued frame describes a moment that is no longer coming.
    feed.forget(one);
    CHECK(feed.take(60.0).empty());

    // And the track is still the audible one afterwards, so what is produced
    // next arrives normally rather than being held.
    feed.post(one, 3.0, blob(3));
    const auto due = feed.take(60.0);
    REQUIRE(due.size() == 1);
    CHECK(first(due[0]) == 3);
}

TEST_CASE("an undrained panel does not grow without bound", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // Nobody is draining -- paused, or the display was hidden -- and the
    // decoder keeps producing. What is kept is the recent history, not all of
    // it: two minutes at two hundred frames a second is already 24,000 states.
    const Url one = track("long.mid");
    feed.setAudibleTrack(one);
    for (int i = 0; i < 400; ++i) {
        feed.post(one, static_cast<double>(i), blob(static_cast<unsigned char>(i)));
    }

    const auto due = feed.take(1000.0);
    CHECK(due.size() < 400);
    REQUIRE_FALSE(due.empty());
    // The newest is always kept; it is the oldest that goes.
    CHECK(due.back().seconds == 399.0);
    CHECK(due.front().seconds >= 399.0 - 120.0);
}

TEST_CASE("switching the display off stops the feed and empties it",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    CHECK_FALSE(feed.wanted());
    feed.setWanted(true);
    CHECK(feed.wanted());

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));

    // A producer reads wanted() and stops producing; what was already queued
    // describes a panel nobody is looking at any more.
    feed.setWanted(false);
    CHECK_FALSE(feed.wanted());
    CHECK(feed.take(60.0).empty());
}

TEST_CASE("closing and reopening the display during a track still shows it",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    const Url one = track("one.mid");
    feed.setWanted(true);
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));
    CHECK(feed.take(60.0).size() == 1);

    // Switching off drops what was queued -- nobody is looking at it. What it
    // must not drop is *which track is playing*: nothing else ever says so
    // except a track beginning, so forgetting it here left a panel that had
    // been closed and reopened mid-track empty until the next track started.
    feed.setWanted(false);
    feed.setWanted(true);

    feed.post(one, 2.0, blob(2));
    const auto due = feed.take(60.0);
    REQUIRE(due.size() == 1);
    CHECK(first(due[0]) == 2);

    feed.setWanted(false);
    feed.clear();
}

TEST_CASE("a display can tell 'nothing produces one' from 'nothing yet'",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();
    feed.setWanted(true);

    // The two look identical on screen -- a blank panel -- and mean completely
    // different things: one is a track on a synthesiser with no display at all.
    CHECK_FALSE(feed.producing());

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));
    CHECK(feed.producing());

    feed.setWanted(false);
    CHECK_FALSE(feed.producing());
    feed.clear();
}

TEST_CASE("a display opening late has something to show at once", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // The decoder runs far ahead of the speaker -- a synthesiser renders much
    // faster than real time and the engine buffers deeply -- so a panel opened
    // part-way through a track finds everything queued at a position that has
    // not been reached. Waiting for one to fall due leaves it blank for
    // seconds, which is what it looked like on Kevin's machine.
    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 30.0, blob(1));
    feed.post(one, 31.0, blob(2));

    CHECK(feed.take(2.0).empty());

    const auto seed = feed.peekEarliest();
    REQUIRE(seed.has_value());
    CHECK(first(*seed) == 1);

    // Peeked, not taken: the frame is still there to be drained on time, and
    // showing it early must not consume it.
    const auto due = feed.take(60.0);
    REQUIRE(due.size() == 2);
    CHECK(first(due[0]) == 1);
}

TEST_CASE("there is nothing to seed a display with before a track is known",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();
    feed.post(track("early.mid"), 1.0, blob(1));
    CHECK_FALSE(feed.peekEarliest().has_value());
}
