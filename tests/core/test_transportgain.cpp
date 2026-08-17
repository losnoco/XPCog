// TransportGain, the output stage's volume-times-fade multiplier.
//
// Worth its own file because this arithmetic used to exist twice -- in the real
// device's callback and in the offline test double -- and the copies disagreed.
// The device's skipped its work whenever the ramp had settled, which is right at
// unity and catastrophic at zero: a faded pause left both level and target at
// zero, so the next buffer went out untouched, at full volume. Then rampGain made
// the two differ again and the gain ramped up from zero, having already been
// heard.
//
// No test could catch that, because the only reachable implementation in a test
// was the correct one. So the fix was to have a single implementation, and these
// are its tests -- including, first, the exact case that was broken.

#include "xpcog/core/audio/TransportGain.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using Catch::Approx;
using xpcog::TransportGain;

namespace {

/// One second of full-scale samples, so any gain applied is read straight off the
/// output.
std::vector<float> ones(std::size_t frames, std::size_t channels) {
    return std::vector<float>(frames * channels, 1.0F);
}

}  // namespace

TEST_CASE("a fade settled at zero is silence, not a skipped multiply",
          "[audio][gain]") {
    // The regression. Ramp to zero over a single frame's worth of time so the
    // level lands immediately, then hand it a fresh buffer: what comes back must
    // be silent. The bug returned the buffer unchanged, because level == target
    // was read as "nothing to do".
    TransportGain gain;
    gain.rampTo(0.0F, 0.0, 48000.0);  // zero length: snap

    std::vector<float> first = ones(4, 2);
    gain.apply(first.data(), first.size(), 2, 1.0F);
    REQUIRE(gain.ramping() == false);
    REQUIRE(gain.level() == 0.0F);

    std::vector<float> second = ones(64, 2);
    gain.apply(second.data(), second.size(), 2, 1.0F);
    for (const float sample : second) {
        REQUIRE(sample == 0.0F);
    }
}

TEST_CASE("a settled unity fade leaves the buffer untouched", "[audio][gain]") {
    // The other half of the same condition: at unity there really is nothing to
    // do, and the buffer must come back bit-identical rather than multiplied by a
    // computed 1.0.
    TransportGain      gain;
    std::vector<float> samples{0.25F, -0.5F, 0.125F, 1.0F};
    const std::vector<float> before = samples;

    gain.apply(samples.data(), samples.size(), 2, 1.0F);
    REQUIRE(samples == before);
}

TEST_CASE("volume applies with no ramp outstanding", "[audio][gain]") {
    // Volume alone must still reach the samples. It is a separate multiplier from
    // the fade, and the skip test has to account for it -- an early return that
    // only looked at the fade would play everything at full volume whenever the
    // user had turned it down.
    TransportGain      gain;
    std::vector<float> samples = ones(8, 2);

    gain.apply(samples.data(), samples.size(), 2, 0.5F);
    for (const float sample : samples) {
        REQUIRE(sample == Approx(0.5F));
    }
}

TEST_CASE("the ramp advances per frame, not per sample", "[audio][gain]") {
    // 100 frames to travel from 0 to 1. After exactly 100 frames of stereo the
    // level must be 1, not 1 reached in 50 -- a ramp stepped per sample would
    // arrive at twice the speed on stereo, and at eight times on 7.1.
    TransportGain gain;
    gain.rampTo(0.0F, 0.0, 48000.0);
    std::vector<float> settle = ones(1, 2);
    gain.apply(settle.data(), settle.size(), 2, 1.0F);
    REQUIRE(gain.level() == 0.0F);

    constexpr std::size_t kFrames = 100;
    gain.rampTo(1.0F, 1000.0 * kFrames / 48000.0, 48000.0);

    std::vector<float> half = ones(kFrames / 2, 2);
    gain.apply(half.data(), half.size(), 2, 1.0F);
    REQUIRE(gain.level() == Approx(0.5).margin(0.02));

    std::vector<float> rest = ones(kFrames / 2, 2);
    gain.apply(rest.data(), rest.size(), 2, 1.0F);
    REQUIRE(gain.level() == Approx(1.0).margin(0.02));

    // Arrival takes a frame or two longer than the arithmetic suggests, and that
    // is a property of the ramp rather than a fault in it: the step is 1/100 as a
    // float, which is a shade under, so a hundred of them sum to 0.99999998 and
    // the clamp has nothing to clamp. The next frame's step crosses 1.0 and lands
    // exactly.
    //
    // Harmless where it matters -- stop() waits on ramping() against a deadline
    // and audio keeps flowing meanwhile -- but worth pinning rather than papering
    // over, because "the ramp lands" and "the ramp lands on the frame you
    // calculated" are different claims and only the first is true.
    REQUIRE(gain.ramping() == true);

    std::vector<float> settled = ones(4, 2);
    gain.apply(settled.data(), settled.size(), 2, 1.0F);
    REQUIRE(gain.level() == 1.0F);
    REQUIRE(gain.ramping() == false);
}

TEST_CASE("every channel of a frame shares one gain", "[audio][gain]") {
    // Within a frame the channels must be multiplied by the same value. Stepping
    // inside the channel loop would put the left and right of one frame at
    // different gains, which skews the stereo image for as long as a ramp runs.
    TransportGain gain;
    gain.rampTo(0.0F, 0.0, 48000.0);
    std::vector<float> settle = ones(1, 2);
    gain.apply(settle.data(), settle.size(), 2, 1.0F);

    gain.rampTo(1.0F, 10.0, 48000.0);
    std::vector<float> samples = ones(64, 2);
    gain.apply(samples.data(), samples.size(), 2, 1.0F);

    for (std::size_t frame = 0; frame < 64; ++frame) {
        INFO("frame " << frame);
        REQUIRE(samples[frame * 2] == samples[(frame * 2) + 1]);
    }
}

TEST_CASE("reset clears a ramp the device is not running", "[audio][gain]") {
    // Why reset() exists: a faded stop leaves the level at zero, and the engine's
    // play() stops before it starts. Without this, every track after the first
    // played to a gain of zero -- silently, and for its whole duration.
    TransportGain gain;
    gain.rampTo(0.0F, 0.0, 48000.0);
    std::vector<float> settle = ones(1, 2);
    gain.apply(settle.data(), settle.size(), 2, 1.0F);
    REQUIRE(gain.level() == 0.0F);

    gain.reset();
    REQUIRE(gain.level() == 1.0F);
    REQUIRE(gain.ramping() == false);

    std::vector<float> samples = ones(8, 2);
    gain.apply(samples.data(), samples.size(), 2, 1.0F);
    for (const float sample : samples) {
        REQUIRE(sample == 1.0F);
    }
}
