// The played-time accumulator and its two thresholds.
//
// Everything here is driven by calling advance() with numbers, which is the
// whole reason PlayMonitor takes the engine's clock rather than reading one: a
// test for "four minutes have been listened to" that actually waited four
// minutes would not be run.
//
// The cases that matter are the ones separating "how much was heard" from "where
// the playhead is". A seek does not move the engine's clock at all, so it cannot
// advance this -- which is the property Cog needs a five-second delta filter to
// approximate and this gets for free. See PlayMonitor.hpp.

#include "xpcog/core/PlayMonitor.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace xpcog;

namespace {

struct Counters {
    int playCount = 0;
    int scrobble  = 0;
};

/// A monitor wired to counters, so each test reads as a sequence of clock
/// advances and a pair of expectations.
[[nodiscard]] PlayMonitor monitorFor(Counters& counters) {
    PlayMonitor monitor;
    monitor.onPlayCountReached([&counters] { counters.playCount += 1; });
    monitor.onScrobbleReached([&counters] { counters.scrobble += 1; });
    return monitor;
}

}  // namespace

TEST_CASE("A three-minute track scrobbles at half its length", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 180.0);
    CHECK(monitor.scrobbleThreshold() == 90.0);

    monitor.advance(89.0);
    CHECK(counters.scrobble == 0);
    // The play count fires much earlier, at sixty seconds, and has already gone.
    CHECK(counters.playCount == 1);

    monitor.advance(90.0);
    CHECK(counters.scrobble == 1);

    // Neither fires twice, however long it runs.
    monitor.advance(180.0);
    CHECK(counters.scrobble == 1);
    CHECK(counters.playCount == 1);
}

TEST_CASE("A long track scrobbles at four minutes, not at half", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    // Twenty minutes. Half would be 600 seconds; the cap is what applies.
    monitor.beginTrack(0.0, 1200.0);
    CHECK(monitor.scrobbleThreshold() == 240.0);

    monitor.advance(239.0);
    CHECK(counters.scrobble == 0);
    monitor.advance(240.0);
    CHECK(counters.scrobble == 1);
}

TEST_CASE("A track under thirty seconds is never scrobbled", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 29.0);
    CHECK(monitor.scrobbleThreshold() == 0.0);

    // Played far past its own length -- a looping chiptune, say. Still nothing:
    // a zero threshold means never, not immediately, and that distinction is the
    // one a naive `played >= threshold` gets wrong.
    monitor.advance(600.0);
    CHECK(counters.scrobble == 0);
    // The play count is not subject to the same rule and does fire.
    CHECK(counters.playCount == 1);
}

TEST_CASE("A track with no duration is never scrobbled", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    // A live stream. Cog refuses these the same way, and the reason is that half
    // of an unknown length is not four minutes -- it is unknown.
    monitor.beginTrack(0.0, 0.0);
    CHECK(monitor.scrobbleThreshold() == 0.0);

    monitor.advance(3600.0);
    CHECK(counters.scrobble == 0);
}

TEST_CASE("Only audible time counts, so a seek advances nothing",
          "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 300.0);
    CHECK(monitor.scrobbleThreshold() == 150.0);

    // Ten seconds heard, then the listener seeks to 4:50. The engine's clock
    // counts frames delivered to the device, so it does not move for a seek --
    // there is no call to make here at all, which is the point. Another ten
    // seconds of listening follows.
    monitor.advance(10.0);
    monitor.advance(20.0);

    CHECK(monitor.playedSeconds() == 20.0);
    CHECK(counters.scrobble == 0);
    CHECK(counters.playCount == 0);
}

TEST_CASE("A new track starts its own count", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 300.0);
    monitor.advance(100.0);
    CHECK(counters.playCount == 1);

    // The seam. The engine's clock keeps running across it -- it is one device
    // stream -- so the new track's base is wherever it had got to.
    monitor.beginTrack(100.0, 300.0);
    CHECK(monitor.playedSeconds() == 0.0);
    CHECK(monitor.playCountReported() == false);

    monitor.advance(150.0);
    CHECK(monitor.playedSeconds() == 50.0);
    CHECK(counters.playCount == 1);  // 50 seconds in: not yet

    monitor.advance(161.0);
    CHECK(counters.playCount == 2);
}

TEST_CASE("A restarted engine clock does not credit the new track",
          "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 300.0);
    monitor.advance(500.0);
    // That first track really was listened to past its 150-second threshold, so
    // it has scrobbled. What matters below is that the *second* one does not.
    REQUIRE(counters.scrobble == 1);

    // stop() then play() restarts the engine's count at zero. Without the
    // backwards guard the next advance would compute a delta of -500 and then a
    // huge positive one, and this track would scrobble on its first tick.
    monitor.beginTrack(500.0, 300.0);
    monitor.advance(0.0);
    CHECK(monitor.playedSeconds() == 0.0);

    monitor.advance(10.0);
    CHECK(monitor.playedSeconds() == 10.0);
    CHECK(counters.scrobble == 1);
}

TEST_CASE("Nothing accumulates while no track is audible", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 300.0);
    monitor.advance(50.0);
    monitor.clear();

    // The clock keeps moving -- another track is decoding, or the device is
    // draining -- and none of it belongs to anything.
    monitor.advance(500.0);
    CHECK(counters.playCount == 0);
    CHECK(counters.scrobble == 0);

    // And the next track re-bases from there rather than inheriting it.
    monitor.beginTrack(500.0, 300.0);
    monitor.advance(510.0);
    CHECK(monitor.playedSeconds() == 10.0);
}

TEST_CASE("A short track can cross both thresholds at once", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    // 90 seconds: half is 45, which is below the play count's 60. So the
    // scrobble threshold is crossed first, and a single large advance crosses
    // both -- which is what a tick delayed by a stalled interface looks like.
    monitor.beginTrack(0.0, 90.0);
    CHECK(monitor.scrobbleThreshold() == 45.0);

    monitor.advance(70.0);
    CHECK(counters.scrobble == 1);
    CHECK(counters.playCount == 1);
}

TEST_CASE("A track looping under repeat-one is counted once", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    // Three minutes, played through.
    monitor.beginTrack(0.0, 180.0);
    monitor.advance(180.0);
    CHECK(counters.playCount == 1);
    CHECK(counters.scrobble == 1);

    // Repeat-one hands the same entry back, so the seam announces a track that
    // never stopped. The engine's clock runs on across it -- that is what says
    // this is a lap rather than the listener starting the track again.
    monitor.repeatTrack(180.0, 180.0);
    monitor.advance(360.0);
    CHECK(counters.playCount == 1);

    // The scrobble does fire again, and that is the deliberate half: Last.fm
    // counts a repeat as a listen, while the library's tally is asking how many
    // tracks were listened to rather than how long one was left running.
    CHECK(counters.scrobble == 2);

    // And on, for as long as it is left looping.
    monitor.repeatTrack(360.0, 180.0);
    monitor.advance(540.0);
    CHECK(counters.playCount == 1);
    CHECK(counters.scrobble == 3);
}

TEST_CASE("Starting the same track over again counts again", "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    monitor.beginTrack(0.0, 180.0);
    monitor.advance(180.0);
    CHECK(counters.playCount == 1);

    // The listener pressed play on the track that was already playing. A new
    // play() restarts the engine's clock, and a clock that went backwards is
    // the one thing that tells this apart from a repeat-one seam.
    monitor.repeatTrack(0.0, 180.0);
    monitor.advance(60.0);
    CHECK(counters.playCount == 2);
}

TEST_CASE("A repeat lap of a short track never reaches the threshold",
          "[playmonitor]") {
    Counters    counters;
    PlayMonitor monitor = monitorFor(counters);

    // Forty seconds, so a lap is never a minute of listening. Cog would not
    // count this either -- its accumulator restarts per stream as well -- and
    // adding laps together would count a play of a track nobody played through
    // once.
    monitor.beginTrack(0.0, 40.0);
    monitor.advance(40.0);
    monitor.repeatTrack(40.0, 40.0);
    monitor.advance(80.0);
    monitor.repeatTrack(80.0, 40.0);
    monitor.advance(120.0);
    CHECK(counters.playCount == 0);
}
