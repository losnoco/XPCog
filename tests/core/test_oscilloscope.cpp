// The oscilloscope's two pure functions: where a window should start so a tone
// holds still, and how a window wider than the screen folds onto it.

#include "xpcog/core/audio/Oscilloscope.hpp"

#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

using Catch::Approx;
using xpcog::foldForDisplay;
using xpcog::triggerOffset;

namespace {

/// `frames` of a sine with `period` samples per cycle, starting at `phase`
/// cycles in.
std::vector<float> sine(std::size_t frames, double period, double phase = 0.0) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = static_cast<float>(
            0.5 * std::sin(xpcog::test::kTwoPi * ((static_cast<double>(i) / period) + phase)));
    }
    return out;
}

}  // namespace

TEST_CASE("the trigger starts a window at a rising zero crossing", "[oscilloscope]") {
    // A 100-sample period; the crossings are at multiples of 100.
    const std::vector<float> samples = sine(1000, 100.0);

    const std::size_t at = triggerOffset(samples, 400);
    // The search begins at 200, which is itself a crossing -- but one the
    // trigger has not seen the negative side of, so it is not armed for it.
    // The next is at 300, and the trigger fires on the first sample out of the
    // hysteresis band above it, which for this slope is the next one.
    CHECK(at >= 300);
    CHECK(at <= 302);
    CHECK(samples[at - 1] < xpcog::kTriggerHysteresis);
    CHECK(samples[at] >= xpcog::kTriggerHysteresis);
}

TEST_CASE("the trigger holds a tone still whatever the window's drift", "[oscilloscope]") {
    // The same tone, ending at different phases: what the window starts on
    // must be the same crossing every time.
    for (double phase = 0.0; phase < 1.0; phase += 0.13) {
        const std::vector<float> samples = sine(1000, 100.0, phase);
        const std::size_t        at      = triggerOffset(samples, 400);
        REQUIRE(at >= 200);
        REQUIRE(at <= 600);
        CHECK(samples[at - 1] < xpcog::kTriggerHysteresis);
        CHECK(samples[at] >= xpcog::kTriggerHysteresis);
        // The first 100 samples from the trigger are one cycle from zero
        // upward, whatever the phase was.
        CHECK(samples[at + 25] == Approx(0.5F).margin(0.04F));
        CHECK(samples[at + 75] == Approx(-0.5F).margin(0.04F));
    }
}

TEST_CASE("the trigger falls back to the newest window when there is no crossing",
          "[oscilloscope]") {
    SECTION("silence") {
        const std::vector<float> samples(1000, 0.0F);
        CHECK(triggerOffset(samples, 400) == 600);
    }
    SECTION("a DC offset") {
        const std::vector<float> samples(1000, 0.3F);
        CHECK(triggerOffset(samples, 400) == 600);
    }
    SECTION("noise inside the hysteresis band") {
        std::vector<float> samples(1000);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = (i % 2 == 0) ? 0.001F : -0.001F;
        }
        CHECK(triggerOffset(samples, 400) == 600);
    }
    SECTION("a period longer than the search") {
        // One crossing at 50, before the search region [200, 600].
        std::vector<float> samples(1000, 0.5F);
        for (std::size_t i = 0; i < 50; ++i) {
            samples[i] = -0.5F;
        }
        CHECK(triggerOffset(samples, 400) == 600);
    }
}

TEST_CASE("the trigger is sane about a window it cannot fill", "[oscilloscope]") {
    const std::vector<float> samples = sine(100, 10.0);
    CHECK(triggerOffset(samples, 400) == 0);
    CHECK(triggerOffset(samples, 0) == 0);
    CHECK(triggerOffset({}, 10) == 0);
    // Exactly one window: no room to search, the newest window is the whole
    // buffer.
    CHECK(triggerOffset(samples, 100) == 0);
}

TEST_CASE("folding keeps each column's extremes", "[oscilloscope]") {
    // 1000 samples into 10 columns: each column covers 100. A ramp up then
    // down within each hundred, so the extremes are known.
    std::vector<float> samples(1000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::size_t within = i % 100;
        samples[i] = (within < 50 ? static_cast<float>(within) : static_cast<float>(100 - within)) / 100.0F;
        if (i >= 500) {
            samples[i] = -samples[i];
        }
    }

    std::vector<std::pair<float, float>> columns(10);
    foldForDisplay(samples, 1.0F, columns);
    CHECK(columns[0].first == Approx(0.0F));
    CHECK(columns[0].second == Approx(0.5F));
    CHECK(columns[7].first == Approx(-0.5F));
    CHECK(columns[7].second == Approx(0.0F));
}

TEST_CASE("folding applies the gain and clamps", "[oscilloscope]") {
    const std::vector<float>             samples{0.25F, -0.25F, 0.75F, -0.75F};
    std::vector<std::pair<float, float>> columns(2);

    foldForDisplay(samples, 2.0F, columns);
    CHECK(columns[0].first == Approx(-0.5F));
    CHECK(columns[0].second == Approx(0.5F));
    CHECK(columns[1].first == Approx(-1.0F));  // 1.5 clamped
    CHECK(columns[1].second == Approx(1.0F));
}

TEST_CASE("folding fewer samples than columns takes the nearest sample", "[oscilloscope]") {
    const std::vector<float>             samples{0.1F, 0.2F, 0.3F, 0.4F};
    std::vector<std::pair<float, float>> columns(8);

    foldForDisplay(samples, 1.0F, columns);
    for (const auto& [low, high] : columns) {
        CHECK(low == high);
    }
    CHECK(columns[0].first == Approx(0.1F));
    CHECK(columns[7].first == Approx(0.4F));

    std::vector<std::pair<float, float>> empty(3, {9.0F, 9.0F});
    foldForDisplay({}, 1.0F, empty);
    CHECK(empty[1].first == 0.0F);
    CHECK(empty[1].second == 0.0F);
}
