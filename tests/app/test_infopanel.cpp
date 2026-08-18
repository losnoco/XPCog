// The info dock's formatting, which is where Cog's derived PlaylistEntry
// accessors ended up.
//
// The panel itself needs a QApplication and a screen; these are free functions
// for exactly that reason. What they encode is not obvious -- a disc number
// changes how a track number is written, a replay gain of 0 dB is a different
// claim from no gain tag at all, and a stream with no length must not report
// 0:00.000 -- so they are worth pinning down.

#include "windows/InfoPanel.hpp"

#include "xpcog/core/TrackProperties.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

using namespace xpcog;
using namespace xpcog::app::info;

TEST_CASE("a track number is written with its disc when there is one",
          "[app][info]") {
    // Cog's -trackText: zero-padded to two digits, and prefixed with the disc
    // only when the disc is known. "1.03" beats "3" on a box set.
    CHECK(trackText(3, 0) == "03");
    CHECK(trackText(3, 1) == "1.03");
    CHECK(trackText(12, 2) == "2.12");
    CHECK(trackText(105, 0) == "105");

    // No track number is blank rather than "00" -- the panel says what is known,
    // and an untagged file knows nothing here.
    CHECK(trackText(0, 0).isEmpty());
    CHECK(trackText(0, 2).isEmpty());
}

TEST_CASE("length keeps the fraction the playlist column drops", "[app][info]") {
    // This is the panel you open to check whether a gapless rip really is
    // 4:07.000, so unlike the playlist column it does not round to the second.
    CHECK(lengthText(247.0) == "4:07.000");
    CHECK(lengthText(247.5) == "4:07.500");
    CHECK(lengthText(59.999) == "0:59.999");
    CHECK(lengthText(3661.25) == "61:01.250");

    // A live stream has no length. Blank says so; 0:00.000 would be a claim.
    CHECK(lengthText(0.0).isEmpty());
    CHECK(lengthText(-1.0).isEmpty());
}

TEST_CASE("replay gain lists only the values the file carries", "[app][info]") {
    ReplayGainInfo gain;
    CHECK(replayGainText(gain).isEmpty());

    gain.trackGain = -7.25F;
    CHECK(replayGainText(gain) == "Track Gain: -7.25 dB");

    // The sign is always shown, because +0.00 dB is a real tag and has to be
    // distinguishable from the file having no tag at all.
    gain.trackGain = 0.0F;
    CHECK(replayGainText(gain) == "Track Gain: +0.00 dB");

    gain.albumGain = 1.5F;
    gain.albumPeak = 0.98765F;
    const QStringList lines = replayGainText(gain).split('\n');
    REQUIRE(lines.size() == 3);
    // Album before track, as Cog orders them.
    CHECK(lines[0] == "Album Gain: +1.50 dB");
    CHECK(lines[1] == "Album Peak: 0.987650");
    CHECK(lines[2] == "Track Gain: +0.00 dB");
}

TEST_CASE("a volume scale of exactly 1 is not a gain", "[app][info]") {
    // Cog's condition, and it matters: unity volume is what a file with no
    // scaling carries, so reporting it would put a line on nearly every track.
    ReplayGainInfo gain;
    gain.volume = 1.0F;
    CHECK(replayGainText(gain).isEmpty());

    gain.volume = 0.5F;
    CHECK(replayGainText(gain) == "Volume Scale: 0.50×");
}

TEST_CASE("play count reports the count even with no dates", "[app][info]") {
    // The count rides on the playlist entry; the dates only exist in the
    // library database, so a build without one still has something to say.
    CHECK(playCountText(0, 0, 0) == "0");
    CHECK(playCountText(7, 0, 0) == "7");

    const QStringList lines = playCountText(7, 1700000000, 1700086400).split('\n');
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "7");
    CHECK(lines[1].startsWith("First seen: "));
    CHECK(lines[2].startsWith("Last played: "));
    // Formatted for the reader's locale rather than as a Unix timestamp, which
    // is the one thing that must not leak through.
    CHECK(!lines[1].contains("1700000000"));
}
