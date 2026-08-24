// The pitch/tempo stage, engine by engine.
//
// Time-stretchers are the worst kind of DSP to test by listening: every engine
// produces plausible-sounding audio at the wrong length, the wrong pitch, or
// with a latency tail glued on, and each of those is a different bug. The two
// measurable invariants are length -- total output must be what the input was
// worth at the tempo, which is exactly the count-in trim's contract -- and
// dominant frequency, which is what separates "tempo without pitch" (the whole
// point of a stretcher) from "resampled" (the whole point of varispeed).
//
// Tolerances are deliberately loose. These are windowed overlap-add engines
// measured with a zero-crossing counter; the tests pin which *kind* of
// transform ran, not its fidelity.

#include "../TestSignal.hpp"

#include "xpcog/core/AudioFormat.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/audio/TimeStretch.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace xpcog;
using Catch::Approx;

namespace {

constexpr double kRate     = 44100.0;
constexpr int    kChannels = 2;
/// What the engine feeds per pass: kDspBlockSamples of stereo.
constexpr std::size_t kFeedFrames = 2048;

[[nodiscard]] AudioFormat format() {
    AudioFormat fmt{};
    fmt.sampleRate = kRate;
    fmt.channels   = kChannels;
    fmt.format     = SampleFormat::F32;
    return fmt;
}

[[nodiscard]] std::vector<float> stereoSine(double seconds, double freq) {
    const auto         frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> samples(frames * kChannels);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(
            0.4 * std::sin(test::kTwoPi * freq * static_cast<double>(i) / kRate));
        samples[(i * kChannels) + 0] = v;
        samples[(i * kChannels) + 1] = v;
    }
    return samples;
}

[[nodiscard]] std::vector<float> leftChannel(const std::vector<float>& interleaved) {
    std::vector<float> mono;
    mono.reserve(interleaved.size() / kChannels);
    for (std::size_t i = 0; i < interleaved.size(); i += kChannels) {
        mono.push_back(interleaved[i]);
    }
    return mono;
}

/// Runs `input` through a freshly-prepared stage in engine-sized blocks,
/// drains, and hands back the interleaved output.
[[nodiscard]] std::vector<float> runThrough(const StretchOptions& options,
                                            const std::vector<float>& input) {
    TimeStretch stretch;
    stretch.prepare(format());
    stretch.setOptions(options);

    std::vector<float> out;
    const std::size_t  frames = input.size() / kChannels;
    for (std::size_t offset = 0; offset < frames; offset += kFeedFrames) {
        const std::size_t step = std::min(kFeedFrames, frames - offset);
        stretch.process(input.data() + (offset * kChannels), step, out);
    }
    stretch.drain(out);
    return out;
}

[[nodiscard]] StretchOptions optionsFor(StretchEngine engine, double tempo,
                                        double pitch) {
    StretchOptions options;
    options.engine = engine;
    options.tempo  = tempo;
    options.pitch  = pitch;
    return options;
}

}  // namespace

TEST_CASE("a disabled stretch stage is bit-transparent", "[timestretch]") {
    const auto input = stereoSine(0.5, 440.0);
    const auto out   = runThrough(optionsFor(StretchEngine::Disabled, 2.0, 2.0), input);
    REQUIRE(out == input);
}

TEST_CASE("the engine name parses as Cog spells it", "[timestretch]") {
    CHECK(StretchOptions::engineFromString("disabled") == StretchEngine::Disabled);
    CHECK(StretchOptions::engineFromString("varispeed") == StretchEngine::Varispeed);
    CHECK(StretchOptions::engineFromString("signalsmith") == StretchEngine::Signalsmith);
    CHECK(StretchOptions::engineFromString("faster") == StretchEngine::RubberbandFaster);
    CHECK(StretchOptions::engineFromString("finer") == StretchEngine::RubberbandFiner);
    // A settings file from a build that grew another engine, or a typo: the
    // honest reading is off, not a guess.
    CHECK(StretchOptions::engineFromString("chipmunk") == StretchEngine::Disabled);
}

TEST_CASE("varispeed moves pitch and tempo as one knob", "[timestretch]") {
    const auto input    = stereoSine(2.0, 440.0);
    const auto inFrames = static_cast<double>(input.size() / kChannels);

    SECTION("double speed") {
        const auto out = runThrough(optionsFor(StretchEngine::Varispeed, 2.0, 1.0), input);
        const auto outFrames = static_cast<double>(out.size() / kChannels);
        CHECK(outFrames == Approx(inFrames / 2.0).epsilon(0.02));
        // Resampling: the tone comes out an octave up. This is the difference
        // between this engine and every other one below.
        CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
              Approx(880.0).margin(20.0));
    }

    SECTION("half speed") {
        const auto out = runThrough(optionsFor(StretchEngine::Varispeed, 0.5, 1.0), input);
        const auto outFrames = static_cast<double>(out.size() / kChannels);
        CHECK(outFrames == Approx(inFrames * 2.0).epsilon(0.02));
        CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
              Approx(220.0).margin(10.0));
    }
}

TEST_CASE("Rubber Band changes tempo without moving pitch", "[timestretch]") {
    const auto input    = stereoSine(2.0, 440.0);
    const auto inFrames = static_cast<double>(input.size() / kChannels);

    const auto engine = GENERATE(StretchEngine::RubberbandFaster,
                                 StretchEngine::RubberbandFiner);

    const auto out       = runThrough(optionsFor(engine, 2.0, 1.0), input);
    const auto outFrames = static_cast<double>(out.size() / kChannels);
    // The count-in trim's contract: never longer than the input was worth, and
    // an engine short of it by more than a trim's width did not stretch.
    CHECK(outFrames <= inFrames / 2.0 + 1.0);
    CHECK(outFrames == Approx(inFrames / 2.0).epsilon(0.05));
    CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
          Approx(440.0).margin(15.0));
}

TEST_CASE("Rubber Band shifts pitch without moving tempo", "[timestretch]") {
    const auto input    = stereoSine(2.0, 440.0);
    const auto inFrames = static_cast<double>(input.size() / kChannels);

    const auto out =
        runThrough(optionsFor(StretchEngine::RubberbandFaster, 1.0, 1.5), input);
    const auto outFrames = static_cast<double>(out.size() / kChannels);
    CHECK(outFrames == Approx(inFrames).epsilon(0.05));
    CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
          Approx(660.0).margin(20.0));
}

TEST_CASE("Signalsmith changes tempo without moving pitch", "[timestretch]") {
    const auto input    = stereoSine(2.0, 440.0);
    const auto inFrames = static_cast<double>(input.size() / kChannels);

    const auto out =
        runThrough(optionsFor(StretchEngine::Signalsmith, 2.0, 1.0), input);
    const auto outFrames = static_cast<double>(out.size() / kChannels);
    CHECK(outFrames <= inFrames / 2.0 + 1.0);
    CHECK(outFrames == Approx(inFrames / 2.0).epsilon(0.05));
    CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
          Approx(440.0).margin(15.0));
}

TEST_CASE("Signalsmith shifts pitch without moving tempo", "[timestretch]") {
    const auto input    = stereoSine(2.0, 440.0);
    const auto inFrames = static_cast<double>(input.size() / kChannels);

    const auto out =
        runThrough(optionsFor(StretchEngine::Signalsmith, 1.0, 1.5), input);
    const auto outFrames = static_cast<double>(out.size() / kChannels);
    CHECK(outFrames == Approx(inFrames).epsilon(0.05));
    CHECK(test::dominantFrequency(leftChannel(out), kRate) ==
          Approx(660.0).margin(25.0));
}

TEST_CASE("a tempo change mid-stream is absorbed live", "[timestretch]") {
    // One second at unity, then one second at double speed, with no reset in
    // between: the output should be about a second and a half. A stage that
    // rebuilt its engine on every slider move would land here too, but with a
    // click; a stage that ignored the change lands at two seconds or one.
    const auto input = stereoSine(2.0, 440.0);

    TimeStretch stretch;
    stretch.prepare(format());
    stretch.setOptions(optionsFor(StretchEngine::RubberbandFaster, 1.0, 1.0));

    std::vector<float> out;
    const std::size_t  frames = input.size() / kChannels;
    const std::size_t  half   = frames / 2;
    for (std::size_t offset = 0; offset < half; offset += kFeedFrames) {
        const std::size_t step = std::min(kFeedFrames, half - offset);
        stretch.process(input.data() + (offset * kChannels), step, out);
    }
    stretch.setOptions(optionsFor(StretchEngine::RubberbandFaster, 2.0, 1.0));
    for (std::size_t offset = half; offset < frames; offset += kFeedFrames) {
        const std::size_t step = std::min(kFeedFrames, frames - offset);
        stretch.process(input.data() + (offset * kChannels), step, out);
    }
    stretch.drain(out);

    const auto outFrames = static_cast<double>(out.size() / kChannels);
    CHECK(outFrames == Approx(static_cast<double>(half) * 1.5).epsilon(0.06));
}

TEST_CASE("reset drops the counters with the engine", "[timestretch]") {
    const auto input = stereoSine(0.5, 440.0);

    TimeStretch stretch;
    stretch.prepare(format());
    stretch.setOptions(optionsFor(StretchEngine::Varispeed, 2.0, 1.0));

    std::vector<float> out;
    stretch.process(input.data(), input.size() / kChannels, out);
    REQUIRE(stretch.framesConsumed() > 0);

    stretch.reset();
    CHECK(stretch.framesConsumed() == 0);
    CHECK(stretch.framesProduced() == 0);
}

// ---------------------------------------------------------------------------
// Through the whole engine, offline
// ---------------------------------------------------------------------------

namespace {

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// Plays silence://`seconds` through the real engine with the given stretch
/// settings and returns the rendered length in seconds. Deterministic,
/// device-free, and exactly the path the DSP thread's stretch integration and
/// the end-of-stream drain handshake run on -- a hang here is the drain
/// handshake lost, which is why this test exists beyond the arithmetic.
[[nodiscard]] double renderedSeconds(const char* engine, double tempo) {
    RingBuffer ring(static_cast<std::size_t>(kRate * 0.5) * kChannels);
    auto       output = makeOfflineOutput(ring);

    auto     store = makeMemorySettingsStore();
    Settings settings(*store);
    settings.setRubberbandEngine(engine);
    settings.setTempo(tempo);
    settings.setPitch(tempo);

    AudioEngine engine_(registry(), *output, ring, settings);
    const auto  url = Url::parse("silence://4");
    REQUIRE(url.has_value());
    REQUIRE(engine_.play(*url));
    engine_.waitUntilFinished();
    engine_.stop();

    const auto samples = capturedAudio(*output);
    return static_cast<double>(samples.size() / kChannels) / kRate;
}

}  // namespace

TEST_CASE("the engine renders a track at the configured tempo", "[timestretch]") {
    SECTION("varispeed at double speed") {
        CHECK(renderedSeconds("varispeed", 2.0) == Approx(2.0).margin(0.1));
    }
    SECTION("rubberband at double speed") {
        CHECK(renderedSeconds("faster", 2.0) == Approx(2.0).margin(0.1));
    }
    SECTION("disabled plays at unity whatever the sliders hold") {
        CHECK(renderedSeconds("disabled", 2.0) == Approx(4.0).margin(0.1));
    }
}
