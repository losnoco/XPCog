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

TEST_CASE("the state shown is the one that was current at that moment",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 0.5, blob(1));
    feed.post(one, 1.5, blob(2));
    feed.post(one, 2.5, blob(3));

    // The newest at or before the moment being heard -- not the oldest not yet
    // shown, which is what a queue would give. A panel holds its last state
    // until something changes it, so "what did it look like at 2.0" is the
    // state set at 1.5.
    auto now = feed.stateAt(2.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 2);

    // Asked again at the same moment, the same answer: this is a lookup, and
    // a display that repaints thirty times a second must not consume history.
    now = feed.stateAt(2.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 2);

    now = feed.stateAt(60.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 3);
}

TEST_CASE("a display opening late finds the moment being heard", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // This is the case that made the panel look broken. States are recorded
    // from the start of the track, so a display opened thirty seconds in has
    // thirty seconds of history to look back into and answers immediately --
    // where draining would have handed it whatever the decoder produced next,
    // from wherever it had run ahead to, and then sat frozen until the speaker
    // caught up.
    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    for (int i = 0; i < 40; ++i) {
        feed.post(one, static_cast<double>(i), blob(static_cast<unsigned char>(i)));
    }

    const auto now = feed.stateAt(30.4);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 30);
}

TEST_CASE("before the first state there is still something to show",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // Only the opening moment of a track: the machine has booted and the music
    // has not changed the panel yet. The oldest is the best answer there is,
    // and it beats a blank window.
    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 5.0, blob(7));

    const auto now = feed.stateAt(0.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 7);
}

TEST_CASE("a gapless seam does not show the wrong track's panel", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // The engine opens the next track's decoder "while its audio is still
    // playing out", so both are producing at once and both count from zero
    // within themselves. Without the keying, the second track's states would be
    // read against the first track's position.
    const Url playing = track("playing.mid");
    const Url next    = track("next.mid");
    feed.setAudibleTrack(playing);

    feed.post(playing, 1.0, blob(10));
    feed.post(next, 0.1, blob(20));
    feed.post(next, 0.2, blob(21));

    auto now = feed.stateAt(5.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 10);

    feed.setAudibleTrack(next);
    now = feed.stateAt(5.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 21);
}

TEST_CASE("nothing is shown before the speaker has reached a track",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    feed.post(track("early.mid"), 1.0, blob(1));
    CHECK_FALSE(feed.stateAt(60.0).has_value());
    CHECK_FALSE(feed.producing());
}

TEST_CASE("a seek drops the history it skipped past", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));
    feed.post(one, 2.0, blob(2));

    // Every recorded state describes a moment that is no longer coming.
    feed.forget(one);
    CHECK_FALSE(feed.stateAt(60.0).has_value());

    // The track is still the audible one, so what is recorded next arrives
    // normally rather than being held.
    feed.post(one, 3.0, blob(3));
    const auto now = feed.stateAt(60.0);
    REQUIRE(now.has_value());
    CHECK(first(*now) == 3);
}

TEST_CASE("history does not grow without bound", "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // Nobody is looking -- the panel is closed -- and the decoder keeps
    // recording, because that history is the whole point. What is kept is the
    // recent past, not all of it.
    const Url one = track("long.mid");
    feed.setAudibleTrack(one);
    for (int i = 0; i < 400; ++i) {
        feed.post(one, static_cast<double>(i), blob(static_cast<unsigned char>(i)));
    }

    // Far enough back to have been trimmed: the answer is the oldest kept.
    const auto old = feed.stateAt(10.0);
    REQUIRE(old.has_value());
    CHECK(old->seconds >= 399.0 - 120.0);
}

TEST_CASE("a display can tell 'no panel at all' from 'nothing yet'",
          "[panelfeed]") {
    PanelFeed& feed = PanelFeed::instance();
    feed.clear();

    // The two look identical on screen -- a blank panel -- and mean completely
    // different things: one is a track on a synthesiser with no display.
    CHECK_FALSE(feed.producing());

    const Url one = track("one.mid");
    feed.setAudibleTrack(one);
    feed.post(one, 1.0, blob(1));
    CHECK(feed.producing());
}
