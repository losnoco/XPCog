// The spectrum analyser and the tap that feeds it.
//
// The analyser is the kind of code that always looks like it works: feed it music,
// get bars that move. So these tests check the things a moving bar cannot tell you
// apart -- whether the band a tone lights up is the *right* band, whether full scale
// really is 0 dB and the floor really is 80 dB below it, and whether the tap hands
// back the newest audio rather than the oldest.

#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using Catch::Approx;
using xpcog::AudioTap;
using xpcog::SpectrumAnalyzer;

namespace {

constexpr double kRate = 44100.0;

/// A window of a full-scale sine at `frequency`.
std::vector<float> sineWindow(double frequency, double amplitude = 1.0) {
    std::vector<float> out(SpectrumAnalyzer::kWindowFrames);
    for (std::size_t index = 0; index < out.size(); ++index) {
        const double phase = 2.0 * std::numbers::pi * frequency *
                             static_cast<double>(index) / kRate;
        out[index] = static_cast<float>(amplitude * std::sin(phase));
    }
    return out;
}

/// Which band is loudest.
std::size_t loudestBand(const SpectrumAnalyzer& analyzer) {
    const std::vector<float>& bands = analyzer.bands();
    return static_cast<std::size_t>(
        std::distance(bands.begin(), std::max_element(bands.begin(), bands.end())));
}

}  // namespace

TEST_CASE("the band series sits on musical notes", "[audio][spectrum]") {
    // Cog's bars are a tempered scale, not equal log divisions: C0 * 2^(i/24) taking
    // every second quarter-tone, so one bar per semitone. Checked against the notes
    // themselves rather than against the formula that generated them -- A4 is 440 Hz
    // in any correct tempered scale, and if the ratio or the C0 constant were wrong
    // this is where it shows.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const std::vector<double>& frequencies = analyzer.frequencies();
    REQUIRE_FALSE(frequencies.empty());

    // Ascending, and nothing repeated.
    for (std::size_t index = 1; index < frequencies.size(); ++index) {
        INFO("band " << index);
        REQUIRE(frequencies[index] > frequencies[index - 1]);
    }

    // A semitone apart, throughout: the ratio is the twelfth root of two.
    const double semitone = std::pow(2.0, 1.0 / 12.0);
    for (std::size_t index = 1; index < frequencies.size(); ++index) {
        INFO("band " << index << " at " << frequencies[index]);
        REQUIRE(frequencies[index] / frequencies[index - 1] ==
                Approx(semitone).epsilon(1e-6));
    }

    // And the scale is in tune: some band is A440.
    const bool hasConcertA =
        std::any_of(frequencies.begin(), frequencies.end(), [](double frequency) {
            return std::abs(frequency - 440.0) < 0.1;
        });
    REQUIRE(hasConcertA);
}

TEST_CASE("nothing above Nyquist is offered", "[audio][spectrum]") {
    // Dropped rather than folded. A band above Nyquist has no content to show, and
    // folding one would draw high-frequency energy that is not there.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);
    for (const double frequency : analyzer.frequencies()) {
        REQUIRE(frequency <= kRate / 2.0);
    }

    // A lower rate must offer strictly fewer bands, which is the same rule seen from
    // the other side.
    const std::size_t at44k = analyzer.frequencies().size();
    analyzer.prepare(16000.0);
    REQUIRE(analyzer.frequencies().size() < at44k);
}

TEST_CASE("a tone lights up the band it belongs to", "[audio][spectrum]") {
    // The assertion that matters, and the one a moving display cannot make. A sine
    // at exactly A440 must put its maximum in the band nearest 440 Hz -- not two
    // bands away, which is what a bin-mapping off by one produces and what looks
    // entirely plausible on screen.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    for (const double frequency : {220.0, 440.0, 1000.0, 5000.0}) {
        const std::vector<float> window = sineWindow(frequency);
        analyzer.reset();
        analyzer.analyze(window.data(), window.size());

        const std::size_t peak     = loudestBand(analyzer);
        const double      centre   = analyzer.frequencies()[peak];
        // Within a semitone: the band grid cannot resolve finer, so landing on the
        // nearest band is exact agreement.
        const double      ratio    = std::max(centre / frequency, frequency / centre);
        INFO("a " << frequency << " Hz tone peaked at band " << peak << " ("
                  << centre << " Hz)");
        REQUIRE(ratio < std::pow(2.0, 1.0 / 12.0));
    }
}

TEST_CASE("a full-scale sine reads as full scale", "[audio][spectrum]") {
    // Why the 2/2048 scaling is what it is. A Hamming window sums to about 0.54N, so
    // the scaling is chosen to put a full-scale tone at roughly 0 dBFS -- which is
    // what makes the -80 dB floor mean 80 dB below full scale rather than 80 dB below
    // something arbitrary. If the scaling or the window normalisation drifted, every
    // spectrum would still look fine and the dB axis would be a lie.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const std::vector<float> window = sineWindow(1000.0);
    analyzer.analyze(window.data(), window.size());
    REQUIRE(analyzer.bands()[loudestBand(analyzer)] == Approx(1.0).margin(0.02));

    // And 40 dB down lands halfway up an 80 dB scale.
    analyzer.reset();
    const std::vector<float> quieter = sineWindow(1000.0, 0.01);
    analyzer.analyze(quieter.data(), quieter.size());
    REQUIRE(analyzer.bands()[loudestBand(analyzer)] == Approx(0.5).margin(0.03));
}

TEST_CASE("silence reads as the floor, not as noise", "[audio][spectrum]") {
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const std::vector<float> silence(SpectrumAnalyzer::kWindowFrames, 0.0F);
    analyzer.analyze(silence.data(), silence.size());

    for (const float level : analyzer.bands()) {
        REQUIRE(level == 0.0F);
    }
}

TEST_CASE("a peak holds, then falls", "[audio][spectrum]") {
    // Cog holds a peak for ten frames before letting it drop. Both halves matter: no
    // hold and the marker is indistinguishable from the bar, no decay and it stays at
    // the loudest thing the track ever did.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const std::vector<float> loud    = sineWindow(1000.0);
    const std::vector<float> silence(SpectrumAnalyzer::kWindowFrames, 0.0F);

    analyzer.analyze(loud.data(), loud.size());
    const std::size_t band = loudestBand(analyzer);
    const float       held  = analyzer.peaks()[band];
    REQUIRE(held > 0.9F);

    // Held across the hold window even though the signal has gone.
    for (int frame = 0; frame < 10; ++frame) {
        analyzer.analyze(silence.data(), silence.size());
    }
    REQUIRE(analyzer.bands()[band] == 0.0F);
    REQUIRE(analyzer.peaks()[band] == Approx(held).margin(0.001));

    // Then it comes down.
    for (int frame = 0; frame < 30; ++frame) {
        analyzer.analyze(silence.data(), silence.size());
    }
    REQUIRE(analyzer.peaks()[band] < held);
}

TEST_CASE("the tap hands back the newest audio", "[audio][spectrum]") {
    // The direction is the thing to get right. A window filled from the oldest end
    // shows a spectrum of a second ago, which looks correct and is not -- it just
    // lags. Written as a ramp so the values themselves say which end is which.
    AudioTap tap(1U << 10);
    REQUIRE_FALSE(tap.readLatest(nullptr, 0));

    std::vector<float> out(8, -1.0F);
    REQUIRE_FALSE(tap.readLatest(out.data(), out.size()));  // nothing written yet

    // Mono, ascending, more than the tap holds so it wraps.
    for (int value = 0; value < 2000; ++value) {
        const float sample = static_cast<float>(value);
        tap.write(&sample, 1, 1);
    }

    REQUIRE(tap.readLatest(out.data(), out.size()));
    for (std::size_t index = 0; index < out.size(); ++index) {
        // The last eight written, oldest first: 1992..1999.
        REQUIRE(out[index] == Approx(1992.0F + static_cast<float>(index)));
    }
}

TEST_CASE("the tap mixes channels down by averaging", "[audio][spectrum]") {
    AudioTap tap(1U << 8);

    // One stereo frame, hard left. The average is half, not the left channel and not
    // the maximum -- a display fed only the left channel is wrong in a way nobody
    // notices until a mono-summed check, and one fed the maximum overstates anything
    // hard-panned.
    const float frame[2] = {1.0F, 0.0F};
    tap.write(frame, 2, 2);
    REQUIRE(tap.framesWritten() == 1);

    float latest = -1.0F;
    REQUIRE(tap.readLatest(&latest, 1));
    REQUIRE(latest == Approx(0.5F));
}

TEST_CASE("a short tap zero-pads the front of the window", "[audio][spectrum]") {
    // So a display can draw from the first callback rather than waiting for 4096
    // samples. The padding goes at the front, keeping the newest sample last, which
    // is where the window function's shape assumes it is.
    AudioTap tap(1U << 8);
    const float sample = 0.75F;
    tap.write(&sample, 1, 1);

    std::vector<float> out(4, -1.0F);
    REQUIRE(tap.readLatest(out.data(), out.size()));
    REQUIRE(out[0] == 0.0F);
    REQUIRE(out[1] == 0.0F);
    REQUIRE(out[2] == 0.0F);
    REQUIRE(out[3] == Approx(0.75F));
}

TEST_CASE("clear makes the tap look untouched", "[audio][spectrum]") {
    // For a stop or a track change: the display must not keep the previous track's
    // tail on screen.
    AudioTap    tap(1U << 8);
    const float sample = 1.0F;
    tap.write(&sample, 1, 1);
    tap.clear();

    REQUIRE(tap.framesWritten() == 0);
    float latest = -1.0F;
    REQUIRE_FALSE(tap.readLatest(&latest, 1));
}
