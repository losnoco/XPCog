// The info panel's formatting, which is where Cog's derived PlaylistEntry
// accessors ended up.
//
// The panel itself needs a screen; these are free functions for exactly that
// reason. What they encode is not obvious -- a disc number changes how a track
// number is written, a replay gain of 0 dB is a different claim from no gain tag
// at all, and a stream with no length must not report 0:00.000 -- so they are
// worth pinning down.

#include "InfoPanel.hpp"

#include "xpcog/core/TrackProperties.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::app::info;

namespace {

[[nodiscard]] std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> split;
    std::size_t              start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        split.push_back(text.substr(
            start, newline == std::string::npos ? std::string::npos : newline - start));
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return split;
}

}  // namespace

TEST_CASE("a track number is written with its disc when there is one", "[wx][info]") {
    // Cog's -trackText: zero-padded to two digits, and prefixed with the disc only
    // when the disc is known. "1.03" beats "3" on a box set.
    CHECK(trackText(3, 0) == "03");
    CHECK(trackText(3, 1) == "1.03");
    CHECK(trackText(12, 2) == "2.12");
    CHECK(trackText(105, 0) == "105");

    // No track number is blank rather than "00" -- the panel says what is known,
    // and an untagged file knows nothing here.
    CHECK(trackText(0, 0).empty());
    CHECK(trackText(0, 2).empty());
}

TEST_CASE("length keeps the fraction the playlist column drops", "[wx][info]") {
    // This is the panel you open to check whether a gapless rip really is
    // 4:07.000, so unlike the playlist column it does not round to the second.
    CHECK(lengthText(247.0) == "4:07.000");
    CHECK(lengthText(247.5) == "4:07.500");
    CHECK(lengthText(59.999) == "0:59.999");
    CHECK(lengthText(3661.25) == "61:01.250");

    // A live stream has no length. Blank says so; 0:00.000 would be a claim.
    CHECK(lengthText(0.0).empty());
    CHECK(lengthText(-1.0).empty());
}

TEST_CASE("replay gain lists only the values the file carries", "[wx][info]") {
    ReplayGainInfo gain;
    CHECK(replayGainText(gain).empty());

    gain.trackGain = -7.25F;
    CHECK(replayGainText(gain) == "Track Gain: -7.25 dB");

    // The sign is always shown, because +0.00 dB is a real tag and has to be
    // distinguishable from the file having no tag at all.
    gain.trackGain = 0.0F;
    CHECK(replayGainText(gain) == "Track Gain: +0.00 dB");

    gain.albumGain = 1.5F;
    gain.albumPeak = 0.98765F;
    const std::vector<std::string> split = lines(replayGainText(gain));
    REQUIRE(split.size() == 3);
    // Album before track, as Cog orders them.
    CHECK(split[0] == "Album Gain: +1.50 dB");
    CHECK(split[1] == "Album Peak: 0.987650");
    CHECK(split[2] == "Track Gain: +0.00 dB");
}

TEST_CASE("a volume scale of exactly 1 is not a gain", "[wx][info]") {
    // Cog's condition, and it matters: unity volume is what a file with no scaling
    // carries, so reporting it would put a line on nearly every track.
    ReplayGainInfo gain;
    gain.volume = 1.0F;
    CHECK(replayGainText(gain).empty());

    gain.volume = 0.5F;
    CHECK(replayGainText(gain) == "Volume Scale: 0.50\xC3\x97");
}

TEST_CASE("play count reports the count even with no dates", "[wx][info]") {
    // The count rides on the playlist entry; the dates only exist in the library
    // database, so a build without one still has something to say.
    CHECK(playCountText(0, 0, 0) == "0");
    CHECK(playCountText(7, 0, 0) == "7");

    const std::vector<std::string> split = lines(playCountText(7, 1700000000, 1700086400));
    REQUIRE(split.size() == 3);
    CHECK(split[0] == "7");
    CHECK(split[1].starts_with("First seen: "));
    CHECK(split[2].starts_with("Last played: "));
    // Formatted for the reader rather than as a Unix timestamp, which is the one
    // thing that must not leak through.
    CHECK(split[1].find("1700000000") == std::string::npos);
}
