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
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Approx;
using xpcog::AudioTap;
using xpcog::SpectrumAnalyzer;
using xpcog::TapCursor;

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

TEST_CASE("the floor rescales the display without moving the bands",
          "[audio][spectrum]") {
    // Cog fixes its floor at -80; this is settable, so the mapping from dB to bar
    // height has to follow it. A -40 dB tone sits halfway up an 80 dB scale and
    // three-quarters of the way up a 160 dB one -- and the band it lands in must not
    // change, because the floor is a display choice and the bands are not.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const std::vector<float>  window = sineWindow(1000.0, 0.01);  // -40 dBFS
    const std::vector<double> before = analyzer.frequencies();

    analyzer.analyze(window.data(), window.size());
    const std::size_t band = loudestBand(analyzer);
    REQUIRE(analyzer.bands()[band] == Approx(0.5).margin(0.03));

    analyzer.setFloorDb(-160.0);
    analyzer.analyze(window.data(), window.size());
    REQUIRE(analyzer.floorDb() == -160.0);
    REQUIRE(analyzer.bands()[band] == Approx(0.75).margin(0.03));
    REQUIRE(analyzer.frequencies() == before);

    // A floor at or above full scale would leave nothing to draw, so it is refused
    // rather than clamped -- clamping silently would answer a different question
    // than the caller asked.
    analyzer.setFloorDb(0.0);
    REQUIRE(analyzer.floorDb() == -160.0);
    analyzer.setFloorDb(12.0);
    REQUIRE(analyzer.floorDb() == -160.0);
}

TEST_CASE("frequency mode spreads bars evenly in log frequency",
          "[audio][spectrum]") {
    // Cog's other analyser mode. The property that distinguishes it from the note
    // scale is the *ratio* between neighbours: constant, as in the note scale, but
    // set by the requested bar count rather than by the tempered scale -- so asking
    // for a different count changes the spacing, which it cannot do for notes.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);
    analyzer.setMode(SpectrumAnalyzer::Mode::Frequencies);
    analyzer.setFrequencyBandCount(64);

    const std::vector<double>& frequencies = analyzer.frequencies();
    REQUIRE(frequencies.size() > 8);
    REQUIRE(frequencies.back() <= kRate / 2.0);

    for (std::size_t index = 1; index < frequencies.size(); ++index) {
        INFO("band " << index << " at " << frequencies[index]);
        REQUIRE(frequencies[index] > frequencies[index - 1]);
    }

    // Evenly spaced in the log domain: every step is the same ratio. Compared
    // against the first step rather than against a formula, so this checks the
    // spacing is uniform without restating how it was computed.
    const double step = frequencies[1] / frequencies[0];
    for (std::size_t index = 2; index < frequencies.size(); ++index) {
        INFO("band " << index);
        REQUIRE(frequencies[index] / frequencies[index - 1] ==
                Approx(step).epsilon(0.02));
    }

    // Not the note scale: a semitone is about 1.0595, and 64 bars across eleven
    // octaves are much further apart than that.
    REQUIRE(step > std::pow(2.0, 1.0 / 12.0));
}

TEST_CASE("a tone still lands in the right place in frequency mode",
          "[audio][spectrum]") {
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);
    analyzer.setMode(SpectrumAnalyzer::Mode::Frequencies);
    analyzer.setFrequencyBandCount(128);

    const std::vector<float> window = sineWindow(1000.0);
    analyzer.analyze(window.data(), window.size());

    const double centre = analyzer.frequencies()[loudestBand(analyzer)];
    // Within one band's width of 1 kHz. The bands are wider than semitones here, so
    // the tolerance is the spacing rather than a fixed ratio.
    const double spacing = analyzer.frequencies()[1] / analyzer.frequencies()[0];
    INFO("peaked at " << centre << " Hz with spacing " << spacing);
    REQUIRE(std::max(centre / 1000.0, 1000.0 / centre) < spacing);
}

TEST_CASE("switching modes keeps the band table consistent", "[audio][spectrum]") {
    // The bug this rules out: band data sized for one mode read after switching to
    // the other. bands(), peaks() and frequencies() must always agree in length,
    // whatever order the setters are called in.
    SpectrumAnalyzer analyzer;
    analyzer.prepare(kRate);

    const auto consistent = [&analyzer] {
        REQUIRE(analyzer.bands().size() == analyzer.frequencies().size());
        REQUIRE(analyzer.peaks().size() == analyzer.frequencies().size());
    };
    consistent();

    analyzer.setMode(SpectrumAnalyzer::Mode::Frequencies);
    consistent();
    analyzer.setFrequencyBandCount(200);
    consistent();
    analyzer.setMode(SpectrumAnalyzer::Mode::NoteBands);
    consistent();

    // And a rate change after a mode change still rebuilds against the new mode.
    analyzer.setMode(SpectrumAnalyzer::Mode::Frequencies);
    analyzer.prepare(96000.0);
    consistent();

    const std::vector<float> window = sineWindow(1000.0);
    analyzer.analyze(window.data(), window.size());
    consistent();
}

// ---------------------------------------------------------------------------
// The display's own read cursor
// ---------------------------------------------------------------------------
//
// What these are about: the playback chain does not hand the tap audio at the rate
// a display draws at. It hands over a device period at a time, and if that period
// is large -- several thousand frames -- a display that asks for "the newest
// window" every repaint sees the same samples five or six times and then jumps a
// tenth of a second. TapCursor is what breaks that coupling, so the tests below
// drive a slow, lumpy writer against a fast, even reader and check the reader keeps
// its own time.

namespace {

/// Appends `frames` of a mono ramp whose values are the absolute frame numbers, so
/// the samples a window contains say exactly where in the stream it was taken.
void writeRamp(AudioTap& tap, std::uint64_t& next, std::size_t frames) {
    std::vector<float> chunk(frames);
    for (std::size_t index = 0; index < frames; ++index) {
        chunk[index] = static_cast<float>(next + index);
    }
    tap.write(chunk.data(), frames, 1);
    next += frames;
}

}  // namespace

TEST_CASE("the display cursor advances by its own clock, not the writer's",
          "[audio][spectrum]") {
    // A writer pushing 100 ms chunks against a reader drawing at 60 Hz: the fault
    // this exists for. Following the write head, five of every six windows would be
    // identical to the one before and the sixth would move 4800 samples.
    constexpr double      kSampleRate  = 48000.0;
    constexpr double      kFrame       = 1.0 / 60.0;
    constexpr std::size_t kChunkFrames = 4800;
    constexpr std::size_t kWindow      = 64;

    AudioTap      tap(1U << 14);
    TapCursor     cursor;
    std::uint64_t written = 0;
    cursor.setSampleRate(kSampleRate);

    // A few chunks in already, which is what a display opened part-way through a
    // track sees. The first chunk of a track is its own case -- there is no history
    // to sit behind yet -- and it is covered by the resync tests below.
    for (int chunk = 0; chunk < 3; ++chunk) {
        writeRamp(tap, written, kChunkFrames);
    }
    REQUIRE(tap.writeGranularity() == kChunkFrames);

    std::vector<float> window(kWindow, -1.0F);
    REQUIRE(cursor.read(tap, kFrame, window.data(), window.size()));

    // Six repaints per chunk, over several chunks, with the audio arriving in one
    // lump per six frames the way a device callback does.
    double        secondsSinceChunk = 0.0;
    std::uint64_t previousEnd       = cursor.position();
    for (int frame = 0; frame < 60; ++frame) {
        secondsSinceChunk += kFrame;
        if (secondsSinceChunk >= static_cast<double>(kChunkFrames) / kSampleRate) {
            writeRamp(tap, written, kChunkFrames);
            secondsSinceChunk -= static_cast<double>(kChunkFrames) / kSampleRate;
        }

        REQUIRE(cursor.read(tap, kFrame, window.data(), window.size()));

        // Every frame moves, and moves by a frame's worth of audio -- not by a
        // chunk, and not by nothing.
        const std::uint64_t moved = cursor.position() - previousEnd;
        REQUIRE(moved == static_cast<std::uint64_t>(kSampleRate * kFrame));
        previousEnd = cursor.position();

        // And the samples are the ones at that position: the ramp values say so,
        // which is what rules out a window that merely looks fresh.
        REQUIRE(window.back() == Approx(static_cast<float>(cursor.position() - 1)));
    }

    // A second of that, and not one frame of it was a resync -- the loop above
    // would have caught a jump either way. The cursor is still behind the writer,
    // by somewhere between a frame and a couple of chunks: it closes on the head as
    // a chunk is consumed and is pushed back out when the next one lands, which is
    // the cycle it is supposed to be in. Falling a second behind, or catching the
    // head, are the two ways this goes wrong.
    const std::uint64_t behind = tap.framesWritten() - cursor.position();
    REQUIRE(behind >= 1);
    REQUIRE(behind <= 2 * kChunkFrames);
}

TEST_CASE("small chunks cost the display no extra lag", "[audio][spectrum]") {
    // The lag is one chunk, so a chain that already pushes small buffers pays
    // almost nothing -- which is what keeps this from being a trade rather than a
    // fix.
    constexpr double      kSampleRate  = 48000.0;
    constexpr std::size_t kChunkFrames = 256;

    AudioTap      tap(1U << 14);
    TapCursor     cursor;
    std::uint64_t written = 0;
    cursor.setSampleRate(kSampleRate);

    std::vector<float> window(64, -1.0F);
    for (int chunk = 0; chunk < 40; ++chunk) {
        writeRamp(tap, written, kChunkFrames);
        REQUIRE(cursor.read(tap, static_cast<double>(kChunkFrames) / kSampleRate,
                            window.data(), window.size()));
    }
    REQUIRE(tap.framesWritten() - cursor.position() <= 2 * kChunkFrames);
}

TEST_CASE("the display cursor resyncs rather than drifting", "[audio][spectrum]") {
    constexpr double kSampleRate = 48000.0;

    AudioTap      tap(1U << 12);
    TapCursor     cursor;
    std::uint64_t written = 0;
    cursor.setSampleRate(kSampleRate);

    std::vector<float> window(64, -1.0F);
    writeRamp(tap, written, 1024);
    REQUIRE(cursor.read(tap, 0.0, window.data(), window.size()));

    SECTION("running past a producer that has stalled") {
        // Half a second of frames against a writer that has produced nothing. The
        // cursor cannot advance into audio that does not exist, so it drops back to
        // where the audio is instead of sitting on a position in the future.
        for (int frame = 0; frame < 30; ++frame) {
            REQUIRE(cursor.read(tap, 1.0 / 60.0, window.data(), window.size()));
        }
        REQUIRE(cursor.position() <= tap.framesWritten());
        REQUIRE(window.back() == Approx(static_cast<float>(cursor.position() - 1)));
    }

    SECTION("a producer running faster than real time") {
        // An offline render fills the tap as fast as it can decode. A cursor pacing
        // itself at 1x would fall behind until the samples it wants have been
        // overwritten; it gives up on the position rather than reading whatever is
        // in those slots now.
        for (int chunk = 0; chunk < 20; ++chunk) {
            writeRamp(tap, written, 1024);
            REQUIRE(cursor.read(tap, 1.0 / 60.0, window.data(), window.size()));
            REQUIRE(tap.framesWritten() - cursor.position() <= tap.capacity());
            REQUIRE(window.back() == Approx(static_cast<float>(cursor.position() - 1)));
        }
    }

    SECTION("a track change under the cursor") {
        // clear() puts the tap back to nothing. The cursor must not carry a
        // position from the previous track into the next one.
        tap.clear();
        REQUIRE_FALSE(cursor.read(tap, 1.0 / 60.0, window.data(), window.size()));

        written = 0;
        writeRamp(tap, written, 1024);
        REQUIRE(cursor.read(tap, 1.0 / 60.0, window.data(), window.size()));
        REQUIRE(cursor.position() <= tap.framesWritten());
        REQUIRE(window.back() == Approx(static_cast<float>(cursor.position() - 1)));
    }
}

TEST_CASE("a display with no sample rate still follows the head",
          "[audio][spectrum]") {
    // Nothing has told the cursor what rate the tap is filled at -- a panel drawing
    // before a device has been negotiated. Seconds cannot be turned into frames, so
    // it does what a visualiser did before it existed: the newest window, choppy or
    // not. Never a blank display.
    AudioTap      tap(1U << 12);
    TapCursor     cursor;
    std::uint64_t written = 0;
    writeRamp(tap, written, 512);

    std::vector<float> window(64, -1.0F);
    REQUIRE(cursor.read(tap, 1.0 / 60.0, window.data(), window.size()));
    REQUIRE(window.back() == Approx(511.0F));
}

TEST_CASE("the tap refuses windows it cannot honestly fill", "[audio][spectrum]") {
    AudioTap      tap(1U << 8);
    std::uint64_t written = 0;
    writeRamp(tap, written, 300);  // more than the tap holds, so it has wrapped

    std::vector<float> window(16, -1.0F);
    REQUIRE(tap.readEnding(300, window.data(), window.size()));
    REQUIRE(window.back() == Approx(299.0F));

    // Past the write head is audio that has not been played yet.
    REQUIRE_FALSE(tap.readEnding(301, window.data(), window.size()));
    // And far enough behind it, the slots hold newer audio, not older -- returning
    // them would splice the present into the middle of the past.
    REQUIRE_FALSE(tap.readEnding(20, window.data(), window.size()));
    // Nothing at all is not a window of silence.
    REQUIRE_FALSE(tap.readEnding(0, window.data(), window.size()));
}

TEST_CASE("the tap reports how large the chunks reaching it are",
          "[audio][spectrum]") {
    // This is what tells a display how far behind the head to sit, so it has to
    // rise the moment a large chunk lands -- a reader starved by one needs to know
    // on the next frame -- and to come back down afterwards rather than holding the
    // display back for the rest of the track because of one hiccup.
    AudioTap      tap;
    std::uint64_t written = 0;

    writeRamp(tap, written, 256);
    REQUIRE(tap.writeGranularity() == 256);

    writeRamp(tap, written, 4096);
    REQUIRE(tap.writeGranularity() == 4096);

    for (int chunk = 0; chunk < 2000; ++chunk) {
        writeRamp(tap, written, 256);
    }
    REQUIRE(tap.writeGranularity() == 256);

    tap.clear();
    REQUIRE(tap.writeGranularity() == 0);
}
