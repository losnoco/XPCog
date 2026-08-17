// The DSP chain: DSPNode's contract, and the equaliser kernel.
//
// The equaliser is the one place M4 replaces an Accelerate primitive
// (vDSP_biquadm) with hand-written arithmetic, and a transposed-direct-form slip
// does not announce itself -- a wrong sign on a state update still produces
// filtered-sounding audio. So the central test computes the cascade's transfer
// function directly from the coefficients and compares its magnitude against
// what process() measurably does to a sine. Two independent computations of the
// same filter: one closed-form in the complex plane, one a difference equation
// run sample by sample.
//
// The rest pin down the things that are cheap to get wrong and silent when
// wrong: that flat is bit-transparent rather than merely quiet, that a seek does
// not smear the previous position through the filter state, and that the
// channels do not filter each other.

#include "../TestSignal.hpp"

#include "xpcog/core/AudioFormat.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/Downmix.hpp"
#include "xpcog/core/audio/Fader.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;
using Catch::Approx;

namespace {

constexpr double kRate     = 44100.0;
constexpr int    kChannels = 2;

[[nodiscard]] AudioFormat formatFor(double rate, int channels) {
    AudioFormat format{};
    format.sampleRate = rate;
    format.channels   = static_cast<std::uint32_t>(channels);
    format.format     = SampleFormat::F32;
    return format;
}

/// |H(e^jw)| for one section, straight from the definition.
[[nodiscard]] double sectionMagnitude(const Equalizer::Biquad& biquad, double omega) {
    const std::complex<double> z1 = std::polar(1.0, -omega);
    const std::complex<double> z2 = std::polar(1.0, -2.0 * omega);

    const std::complex<double> numerator   = biquad.b0 + (biquad.b1 * z1) + (biquad.b2 * z2);
    const std::complex<double> denominator = 1.0 + (biquad.a1 * z1) + (biquad.a2 * z2);
    return std::abs(numerator / denominator);
}

/// |H| for the whole 31-section cascade at `frequency`.
[[nodiscard]] double cascadeMagnitude(const Equalizer& equalizer, double frequency) {
    const double omega = 2.0 * xpcog::test::kPi * frequency / kRate;
    double       total = 1.0;
    for (int band = 0; band < Equalizer::kBands; ++band) {
        total *= sectionMagnitude(equalizer.coefficients(band), omega);
    }
    return total;
}

/// Steady-state gain process() applies to a sine at `frequency`, measured as an
/// RMS ratio. The leading `kSettle` frames are discarded because a filter's
/// first output samples are its transient, not its response.
[[nodiscard]] double measuredGain(Equalizer& equalizer, double frequency) {
    constexpr std::size_t kSettle = 8192;
    constexpr std::size_t kFrames = 65536;

    std::vector<float> buffer(kFrames * kChannels);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double t     = static_cast<double>(frame) / kRate;
        const auto   value = static_cast<float>(0.25 * std::sin(xpcog::test::kTwoPi * frequency * t));
        buffer[frame * kChannels]       = value;
        buffer[(frame * kChannels) + 1] = value;
    }
    const std::vector<float> reference = buffer;

    equalizer.reset();
    equalizer.process(buffer.data(), kFrames);

    const auto rms = [](const std::vector<float>& samples) {
        double sum = 0.0;
        for (std::size_t frame = kSettle; frame < kFrames; ++frame) {
            const double value = samples[frame * kChannels];
            sum += value * value;
        }
        return std::sqrt(sum / static_cast<double>(kFrames - kSettle));
    };

    return rms(buffer) / rms(reference);
}

}  // namespace

TEST_CASE("a flat equaliser is inactive and bit-transparent", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));

    REQUIRE_FALSE(equalizer.active());

    // Bit-exact, not merely close. A flat band is mathematically transparent --
    // its numerator and denominator are the same polynomial -- but running 31
    // such sections still rounds, so transparency here is a property of the
    // chain skipping the stage, and that is what is being asserted.
    std::vector<float> buffer;
    buffer.reserve(512);
    for (int index = 0; index < 512; ++index) {
        buffer.push_back(static_cast<float>(std::sin(0.03 * index) * 0.7));
    }
    const std::vector<float> reference = buffer;

    equalizer.process(buffer.data(), buffer.size() / kChannels);
    REQUIRE(buffer == reference);
}

TEST_CASE("a flat band's numerator and denominator are the same polynomial", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));

    // This is *why* 0 dB is transparent, and it is worth stating separately from
    // the bypass: were the coefficient formula wrong, a flat equaliser would
    // still look transparent through active() while colouring anything that
    // turned a single band up.
    for (int band = 0; band < Equalizer::kBands; ++band) {
        const Equalizer::Biquad biquad = equalizer.coefficients(band);
        REQUIRE(biquad.b0 == Approx(1.0));
        REQUIRE(biquad.b1 == Approx(biquad.a1));
        REQUIRE(biquad.b2 == Approx(biquad.a2));
    }
}

TEST_CASE("each band peaks at exactly its requested gain", "[dsp]") {
    // This is the half the cascade test cannot cover. That test compares the
    // difference equation against coefficients() -- both of which shift together
    // if the *formula* is wrong, so it would happily confirm a filter that
    // faithfully implements the wrong biquad.
    //
    // A peaking EQ is defined by |H(w0)| == the requested gain at its own centre,
    // so asserting that checks the coefficients against the definition rather
    // than against a re-derivation of themselves.
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));

    for (const double gainDb : {-12.0, -3.0, 3.0, 9.0}) {
        for (const int band : {5, 17, 26}) {
            equalizer.setBandGain(band, gainDb);

            const double centre = Equalizer::bandFrequencies()[static_cast<std::size_t>(band)];
            const double omega  = 2.0 * xpcog::test::kPi * centre / kRate;
            const double peak   = sectionMagnitude(equalizer.coefficients(band), omega);

            INFO("band " << band << " at " << centre << " Hz, " << gainDb << " dB");
            REQUIRE(20.0 * std::log10(peak) == Approx(gainDb).epsilon(0.0001));

            equalizer.setBandGain(band, 0.0);
        }
    }
}

TEST_CASE("the band Q is the one Cog uses", "[dsp]") {
    // The last parameter neither other test can see. Peak gain is independent of
    // Q, and the cascade test takes its expectation from the same coefficients,
    // so a Q of 1.0 or 2.0 would pass both while making every slider noticeably
    // wider or narrower than Cog's.
    //
    // For an RBJ peaking section, Q is defined as f0 divided by the spacing of
    // the two frequencies at which the boost is half its centre value in dB, so
    // recovering it from the response checks both kQ and the alpha term.
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));

    constexpr int    kBand   = 17;  // 1 kHz
    constexpr double kGainDb = 12.0;
    equalizer.setBandGain(kBand, kGainDb);

    const double centre = Equalizer::bandFrequencies()[kBand];
    const auto   responseDb = [&](double frequency) {
        const double omega = 2.0 * xpcog::test::kPi * frequency / kRate;
        return 20.0 * std::log10(sectionMagnitude(equalizer.coefficients(kBand), omega));
    };

    // Bisect for the half-gain crossing on each side of the centre.
    const auto crossing = [&](double from, double to) {
        for (int step = 0; step < 200; ++step) {
            const double middle = 0.5 * (from + to);
            if (responseDb(middle) < kGainDb / 2.0) {
                from = middle;
            } else {
                to = middle;
            }
        }
        return 0.5 * (from + to);
    };

    const double lower = crossing(20.0, centre);
    const double upper = crossing(20000.0, centre);

    const double recovered = centre / (upper - lower);
    INFO("half-gain points " << lower << " and " << upper << " Hz, Q " << recovered);
    REQUIRE(recovered == Approx(Equalizer::kQ).epsilon(0.02));
    REQUIRE(Equalizer::kQ == 1.4);
}

TEST_CASE("the cascade matches its own transfer function", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));

    // An asymmetric, overlapping setting rather than one lone slider: adjacent
    // peaking sections at Q 1.4 genuinely interact, and a cascade evaluated
    // band-by-band would agree with a sine only if that interaction is carried
    // through correctly.
    equalizer.setBandGain(10, 6.0);    // 200 Hz
    equalizer.setBandGain(11, -4.5);   // 250 Hz
    equalizer.setBandGain(17, 9.0);    // 1 kHz
    equalizer.setBandGain(23, -8.0);   // 4 kHz
    REQUIRE(equalizer.active());

    for (const double frequency : {200.0, 250.0, 1000.0, 4000.0, 630.0, 8000.0}) {
        const double expected = cascadeMagnitude(equalizer, frequency);
        const double measured = measuredGain(equalizer, frequency);
        INFO("at " << frequency << " Hz: expected " << expected << ", measured " << measured);
        REQUIRE(measured == Approx(expected).epsilon(0.01));
    }
}

TEST_CASE("the preamp scales the whole band", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));
    equalizer.setPreamp(-6.0);

    REQUIRE(equalizer.active());
    // Flat bands, so the only gain is the preamp: 10^(-6/20).
    REQUIRE(measuredGain(equalizer, 1000.0) == Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.001));
}

TEST_CASE("reset drops the filter state, so a seek cannot smear", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));
    equalizer.setBandGain(17, 12.0);

    // Something loud enough to leave the resonant sections ringing.
    std::vector<float> loud(2048 * kChannels, 0.9F);
    equalizer.process(loud.data(), 2048);

    // Silence immediately afterwards is *not* silent -- that ringing is the
    // filter's tail, and across a seek it is the previous position bleeding into
    // the new one.
    std::vector<float> tail(512 * kChannels, 0.0F);
    equalizer.process(tail.data(), 512);
    REQUIRE(std::ranges::any_of(tail, [](float value) { return value != 0.0F; }));

    equalizer.reset();
    std::vector<float> afterReset(512 * kChannels, 0.0F);
    equalizer.process(afterReset.data(), 512);
    REQUIRE(std::ranges::all_of(afterReset, [](float value) { return value == 0.0F; }));
}

TEST_CASE("the channels do not filter each other", "[dsp]") {
    Equalizer equalizer;
    equalizer.prepare(formatFor(kRate, kChannels));
    equalizer.setBandGain(17, 12.0);

    // Left driven hard, right digitally silent. Sharing one set of state values
    // across channels -- the obvious way to write this loop -- would leak the
    // left channel into the right and collapse the image, while still sounding
    // like a working equaliser on mono material.
    constexpr std::size_t kFrames = 4096;
    std::vector<float>    buffer(kFrames * kChannels, 0.0F);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double t = static_cast<double>(frame) / kRate;
        buffer[frame * kChannels] =
            static_cast<float>(0.5 * std::sin(xpcog::test::kTwoPi * 1000.0 * t));
    }

    equalizer.process(buffer.data(), kFrames);

    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        REQUIRE(buffer[(frame * kChannels) + 1] == 0.0F);
    }
}

TEST_CASE("bands above Nyquist become identity sections", "[dsp]") {
    Equalizer equalizer;
    // 32 kHz puts the 16 and 20 kHz centres at or past Nyquist.
    equalizer.prepare(formatFor(32000.0, kChannels));
    equalizer.setBandGain(29, 12.0);  // 16 kHz
    equalizer.setBandGain(30, 12.0);  // 20 kHz

    // Identity rather than dropped, so band indices mean the same thing at every
    // sample rate and a stored setting does not shift onto a different slider.
    for (const int band : {29, 30}) {
        const Equalizer::Biquad biquad = equalizer.coefficients(band);
        INFO("band " << band << " at " << Equalizer::bandFrequencies()[band] << " Hz");
        REQUIRE(biquad.b0 == 1.0);
        REQUIRE(biquad.b1 == 0.0);
        REQUIRE(biquad.b2 == 0.0);
        REQUIRE(biquad.a1 == 0.0);
        REQUIRE(biquad.a2 == 0.0);
    }
}

// ---------------------------------------------------------------------------
// The chain as the engine runs it.
//
// Everything above tests the kernel in isolation. What that cannot show is
// whether the engine actually routes audio through it: there are four places
// that fill the converted buffer -- the prebuffer, the normal path and two
// drains -- and a chain wired into three of them would still pass every test
// above while dropping audio past the filter at a seam.
//
// A WAV rather than a FLAC on purpose: no external encoder, so this runs on a
// bare machine and cannot join the sixteen that skip.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path dspFixtureDir() {
    static const std::filesystem::path dir = [] {
        auto path = std::filesystem::temp_directory_path() / "xpcog-dsp-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

/// Two seconds of 1 kHz sine as 16-bit stereo WAV.
std::filesystem::path toneWav() {
    static const std::filesystem::path path = [] {
        const auto out    = dspFixtureDir() / "tone.wav";
        const int  frames = static_cast<int>(kRate) * 2;

        std::vector<std::int16_t> samples;
        samples.reserve(static_cast<std::size_t>(frames) * kChannels);
        for (int frame = 0; frame < frames; ++frame) {
            const double t = static_cast<double>(frame) / kRate;
            const auto   value =
                static_cast<std::int16_t>(12000.0 * std::sin(xpcog::test::kTwoPi * 1000.0 * t));
            samples.push_back(value);
            samples.push_back(value);
        }

        const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
        std::FILE* file      = std::fopen(out.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, file); };
        const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, file); };
        std::fwrite("RIFF", 1, 4, file);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, file);
        u32(16);
        u16(1);
        u16(kChannels);
        u32(static_cast<std::uint32_t>(kRate));
        u32(static_cast<std::uint32_t>(kRate) * kChannels * 2);
        u16(kChannels * 2);
        u16(16);
        std::fwrite("data", 1, 4, file);
        u32(dataBytes);
        std::fwrite(samples.data(), 1, dataBytes, file);
        std::fclose(file);
        return out;
    }();
    return path;
}

const PluginRegistry& dspRegistry() {
    static const PluginRegistry& instance = *[] {
        auto* built = new PluginRegistry;
        registerAllCodecs(*built);
        return built;
    }();
    return instance;
}

/// Renders the tone through a whole engine and returns the captured audio.
std::vector<float> renderWith(double preampDb, int band, double bandDb) {
    RingBuffer ring{static_cast<std::size_t>(kRate * 0.5) * kChannels};
    auto       output = makeOfflineOutput(ring);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setEqPreamp(preampDb);
    if (band >= 0) {
        settings.setRawValue(Equalizer::bandSettingsKeys()[static_cast<std::size_t>(band)],
                             std::to_string(bandDb));
    }

    AudioEngine engine{dspRegistry(), *output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(toneWav())));
    engine.waitUntilFinished();
    engine.stop();
    return capturedAudio(*output);
}

[[nodiscard]] double rmsOf(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    // The first tenth of a second is skipped: it holds the filter's transient,
    // and at a boost of several dB that is louder than the steady state.
    const std::size_t skip = static_cast<std::size_t>(kRate * 0.1) * kChannels;
    if (samples.size() <= skip) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t index = skip; index < samples.size(); ++index) {
        sum += static_cast<double>(samples[index]) * samples[index];
    }
    return std::sqrt(sum / static_cast<double>(samples.size() - skip));
}

}  // namespace

TEST_CASE("the engine renders through the equaliser", "[dsp]") {
    const std::vector<float> flat = renderWith(0.0, -1, 0.0);
    REQUIRE_FALSE(flat.empty());

    // The gain the kernel says a +9 dB band at 1 kHz should produce, so this
    // catches the chain running twice or not at all as readily as it catches it
    // running at the wrong setting -- a bare "output differs" assertion would
    // pass on a double application.
    constexpr int    kBand   = 17;  // 1 kHz
    constexpr double kBandDb = 9.0;
    Equalizer        reference;
    reference.prepare(formatFor(kRate, kChannels));
    reference.setBandGain(kBand, kBandDb);
    const double expected = cascadeMagnitude(reference, 1000.0);

    const std::vector<float> boosted = renderWith(0.0, kBand, kBandDb);
    REQUIRE(boosted.size() == flat.size());

    const double measured = rmsOf(boosted) / rmsOf(flat);
    INFO("expected " << expected << ", measured " << measured);
    REQUIRE(measured == Approx(expected).epsilon(0.02));
}

TEST_CASE("a flat equaliser leaves the engine's output untouched", "[dsp]") {
    // Bit-identical, which is the property that lets the equaliser exist in the
    // signal path at all times without costing anyone who never opens it.
    const std::vector<float> first  = renderWith(0.0, -1, 0.0);
    const std::vector<float> second = renderWith(0.0, 17, 0.0);
    REQUIRE_FALSE(first.empty());
    REQUIRE(first == second);
}

TEST_CASE("the preamp reaches the engine's output", "[dsp]") {
    const std::vector<float> flat     = renderWith(0.0, -1, 0.0);
    const std::vector<float> attenuated = renderWith(-6.0, -1, 0.0);
    REQUIRE(attenuated.size() == flat.size());
    REQUIRE(rmsOf(attenuated) / rmsOf(flat) ==
            Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));
}

TEST_CASE("an equaliser change is heard promptly", "[dsp]") {
    // The regression guard for the bug this design exists to fix. With the chain
    // behind the deep buffer, moving a slider changed nothing audible for about
    // three seconds; the depth now sits ahead of the chain, so the delay is
    // bounded by the shallow ring instead.
    //
    // Measured in audio time rather than wall clock, which is what makes it
    // meaningful under an offline output at all -- and paced, because an
    // unlimited drain would consume the whole file before the change was made.
    constexpr std::size_t kShallowSamples = 1U << 14;  // the app's post-DSP ring
    RingBuffer            ring{kShallowSamples};
    auto                  output = makeOfflineOutput(ring, 8.0);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};

    AudioEngine engine{dspRegistry(), *output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(toneWav())));

    // Let it settle into steady playback before disturbing it.
    for (int spin = 0; spin < 400 && engine.playedSeconds() < 0.4; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double changedAt = engine.playedSeconds();
    REQUIRE(changedAt >= 0.4);

    settings.setRawValue(Equalizer::bandSettingsKeys()[17], "12.0");  // 1 kHz
    engine.reloadDsp();

    engine.waitUntilFinished();
    engine.stop();

    const std::vector<float> captured = capturedAudio(*output);
    REQUIRE_FALSE(captured.empty());

    // The source peaks at about 0.366; a 12 dB boost roughly quadruples it, so
    // twice the original peak is unambiguously past the transition.
    constexpr double      kThreshold = 0.366 * 2.0;
    constexpr std::size_t kWindow    = 64;
    std::size_t           transition = 0;
    for (std::size_t frame = 0; frame + kWindow < captured.size() / kChannels; frame += kWindow) {
        double peak = 0.0;
        for (std::size_t index = 0; index < kWindow; ++index) {
            peak = std::max(peak, std::abs(static_cast<double>(
                                      captured[((frame + index) * kChannels)])));
        }
        if (peak > kThreshold) {
            transition = frame;
            break;
        }
    }
    REQUIRE(transition > 0);

    const double heardAt  = static_cast<double>(transition) / kRate;
    const double latency  = heardAt - changedAt;
    INFO("changed at " << changedAt << " s, heard at " << heardAt << " s, latency "
                       << latency << " s");

    // Bounded well clear of the measurement, not tight against it. It comes out
    // around 0.4 s -- the shallow ring, plus a pump block, plus the filter's own
    // settling time and this test's polling granularity -- and the regression
    // being guarded against is three seconds, so a 1 s bound catches it without
    // turning scheduling noise into a failure.
    REQUIRE(latency < 1.0);
}

// ---------------------------------------------------------------------------
// The fader.
// ---------------------------------------------------------------------------

TEST_CASE("fading disabled leaves the signal alone", "[dsp]") {
    Fader fader;
    fader.prepare(formatFor(kRate, kChannels));
    // enableFading defaults off, and off must mean bit-exact rather than a
    // multiply by 1.0f.
    REQUIRE_FALSE(fader.active());

    std::vector<float> buffer(256, 0.5F);
    const std::vector<float> reference = buffer;
    fader.process(buffer.data(), buffer.size() / kChannels);
    REQUIRE(buffer == reference);
}

TEST_CASE("a fade in ramps from silence over Cog's 200 ms", "[dsp]") {
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));
    REQUIRE(fader.active());
    REQUIRE(fader.level() == 0.0);

    // One second, so the ramp has room to land well inside the buffer.
    const std::size_t frames = static_cast<std::size_t>(kRate);
    std::vector<float> buffer(frames * kChannels, 1.0F);
    fader.process(buffer.data(), frames);

    // It must start at silence and never step backwards: a ramp that overshoots
    // and corrects would be heard as the click it exists to remove.
    REQUIRE(buffer[0] < 0.01F);
    for (std::size_t frame = 1; frame < frames; ++frame) {
        REQUIRE(buffer[frame * kChannels] >= buffer[(frame - 1) * kChannels]);
    }

    // Landing point: 200 ms at this rate, within a frame either way.
    const auto expected = static_cast<std::size_t>(kRate * 0.2);
    std::size_t landed  = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (buffer[frame * kChannels] >= 1.0F) {
            landed = frame;
            break;
        }
    }
    INFO("landed at frame " << landed << ", expected about " << expected);
    REQUIRE(landed == Approx(static_cast<double>(expected)).epsilon(0.01));

    // Once landed it is transparent by value rather than by being skipped: the
    // stage keeps running so its rolling history sees every block -- the tail a
    // future seek fades out has to be gap-free -- but what it emits must be
    // bit-identical to what it was given.
    REQUIRE(fader.level() == 1.0);
    std::vector<float> steady(64 * kChannels, 0.25F);
    const std::vector<float> untouched = steady;
    fader.process(steady.data(), 64);
    REQUIRE(steady == untouched);
}

TEST_CASE("both channels fade together", "[dsp]") {
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    // A per-sample rather than per-frame ramp would advance twice as fast on the
    // right as the left, which is a moving image rather than a fade.
    const std::size_t frames = 1024;
    std::vector<float> buffer(frames * kChannels, 1.0F);
    fader.process(buffer.data(), frames);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        REQUIRE(buffer[frame * kChannels] == buffer[(frame * kChannels) + 1]);
    }
}

TEST_CASE("a seek fades in, and only when fading is enabled", "[dsp]") {
    // reset() is the seam signal the whole chain already receives, so the fader
    // needs no transport plumbing of its own -- this checks that wiring rather
    // than the ramp.
    Fader fader;
    fader.prepare(formatFor(kRate, kChannels));

    fader.reset();
    REQUIRE_FALSE(fader.active());  // disabled: a seek changes nothing

    fader.setEnabled(true);
    fader.reset();
    REQUIRE(fader.active());
    REQUIRE(fader.level() == 0.0);

    // Turning it off mid-ramp must land rather than freeze the signal at a
    // partial gain, which would leave everything quiet until the next seek.
    fader.setEnabled(false);
    REQUIRE(fader.level() == 1.0);
    REQUIRE_FALSE(fader.active());
}

// ---------------------------------------------------------------------------
// The downmix matrix.
// ---------------------------------------------------------------------------

namespace {

/// One frame of 5.1, with `value` on the channel at canonical index `index` and
/// silence everywhere else. 5.1 order is FL FR FC LFE BL BR.
std::vector<float> only(std::uint32_t channel, std::uint32_t channels, float value) {
    std::vector<float> frame(channels, 0.0F);
    frame[channel] = value;
    return frame;
}

}  // namespace

TEST_CASE("front left and right stay on their own side", "[dsp]") {
    // The pair must not cross-mix: Cog mirrors the front ratios so the second
    // entry (zero) is what the opposite channel gets. Getting this wrong collapses
    // the image to mono while still looking like a plausible downmix.
    std::vector<float> out(2, 0.0F);

    auto left = only(0, 6, 1.0F);
    downmixToStereo(left.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] > 0.0F);
    REQUIRE(out[1] == 0.0F);

    auto right = only(1, 6, 1.0F);
    downmixToStereo(right.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] == 0.0F);
    REQUIRE(out[1] > 0.0F);
}

TEST_CASE("centre and LFE go equally to both sides", "[dsp]") {
    std::vector<float> out(2, 0.0F);

    for (const std::uint32_t channel : {2U, 3U}) {  // front centre, LFE
        auto frame = only(channel, 6, 1.0F);
        downmixToStereo(frame.data(), 6, kConfig5Point1, out.data(), 1);
        INFO("channel " << channel);
        REQUIRE(out[0] > 0.0F);
        REQUIRE(out[0] == out[1]);
    }
}

TEST_CASE("the 5.1 ratios are Cog's", "[dsp]") {
    // Pinned to the constants rather than to a property, because the whole point
    // of reproducing Cog's ladder is that a 5.1 file mixes to the same thing. The
    // expected values follow Cog's order of operations: front and centre are set
    // by the back-channel branch, then both scaled by 0.8 for the LFE.
    std::vector<float> out(2, 0.0F);

    auto frontLeft = only(0, 6, 1.0F);
    downmixToStereo(frontLeft.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] == Approx(0.651F * 0.8F).epsilon(1e-5));

    auto centre = only(2, 6, 1.0F);
    downmixToStereo(centre.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] == Approx(0.46F * 0.8F).epsilon(1e-5));

    // LFE takes the centre ratio *after* that attenuation, which is Cog's
    // ordering rather than an independent constant.
    auto lfe = only(3, 6, 1.0F);
    downmixToStereo(lfe.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] == Approx(0.46F * 0.8F).epsilon(1e-5));

    // Back left is asymmetric on purpose: mostly its own side, a little across.
    auto backLeft = only(4, 6, 1.0F);
    downmixToStereo(backLeft.data(), 6, kConfig5Point1, out.data(), 1);
    REQUIRE(out[0] == Approx(0.5636F * 0.8F).epsilon(1e-5));
    REQUIRE(out[1] == Approx(0.3254F * 0.8F).epsilon(1e-5));
}

TEST_CASE("Cog's matrix can exceed full scale, and does", "[dsp]") {
    // Written expecting the ratios to keep a downmix inside unity. They do not:
    // six correlated channels at full scale sum to 1.968 on each side, so the
    // matrix has no headroom guarantee at all. Recorded rather than corrected,
    // because the ratios are Cog's and quietly scaling them would make XPCog
    // disagree with Cog on every surround file.
    //
    // It survives contact with real material because six channels are never
    // identical and at full scale; the constants are a listening compromise, not
    // a bound. Anything downstream that assumes samples are in [-1, 1] needs to
    // know that a 5.1 source can hand it nearly double.
    std::vector<float> frame(6, 1.0F);
    std::vector<float> out(2, 0.0F);
    downmixToStereo(frame.data(), 6, kConfig5Point1, out.data(), 1);

    const float expected = (0.651F * 0.8F)      // front left
                           + (0.46F * 0.8F)     // front centre
                           + (0.46F * 0.8F)     // LFE, which takes the centre ratio
                           + (0.5636F * 0.8F)   // back left
                           + (0.3254F * 0.8F);  // back right, crossing over
    REQUIRE(out[0] == Approx(expected).epsilon(1e-5));
    REQUIRE(out[0] > 1.0F);

    // Symmetric, which is the part that would matter if it were ever clamped.
    REQUIRE(out[0] == Approx(out[1]).epsilon(1e-5));
}

TEST_CASE("mono is the average of the stereo downmix", "[dsp]") {
    std::vector<float> frame{0.4F, -0.2F, 0.1F, 0.0F, 0.3F, 0.05F};
    std::vector<float> stereo(2, 0.0F);
    std::vector<float> mono(1, 0.0F);

    downmixToStereo(frame.data(), 6, kConfig5Point1, stereo.data(), 1);
    downmixToMono(frame.data(), 6, kConfig5Point1, mono.data(), 1);

    REQUIRE(mono[0] == Approx(0.5F * (stereo[0] + stereo[1])).epsilon(1e-6));
}

TEST_CASE("channels the matrix has no place for are dropped", "[dsp]") {
    // Front centre left and right, and everything from the top layer up, fall
    // through Cog's switch to a zero ratio. Reproduced rather than improved on.
    const std::uint32_t config = kConfig5Point1 | kChannelFrontCenterLeft;
    std::vector<float>  out(2, 0.0F);

    // Index 6 within this config is the front-centre-left flag.
    auto frame = only(6, 7, 1.0F);
    downmixToStereo(frame.data(), 7, config, out.data(), 1);
    REQUIRE(out[0] == 0.0F);
    REQUIRE(out[1] == 0.0F);
}

TEST_CASE("a stereo source is left alone by the converter's channel fit", "[dsp]") {
    // Guards the path rather than the matrix: equal counts must not be routed
    // through a downmix at all, or stereo would be scaled by front ratios.
    std::vector<float> out(4, 0.0F);
    std::vector<float> frame{0.5F, -0.25F, 0.5F, -0.25F};
    downmixToStereo(frame.data(), 2, kConfigStereo, out.data(), 2);
    // 2 -> 2 through the matrix *would* be unity here, which is what makes the
    // fast path in fitChannels safe as well as quicker.
    REQUIRE(out[0] == Approx(0.5F).epsilon(1e-6));
    REQUIRE(out[1] == Approx(-0.25F).epsilon(1e-6));
}

TEST_CASE("a seek crossfade never goes quiet in the middle", "[dsp]") {
    // Constant-power legs are chosen for the uncorrelated case, so identical
    // audio either side -- what a very short seek produces -- swells by up to
    // 3 dB instead of holding flat. That is the accepted cost of not *dipping*
    // 3 dB on every ordinary seek, and it is recorded rather than smoothed over.
    // What must hold in both cases is that the level never sags below the input.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    // Settle the track-start fade in first, or the history -- and therefore the
    // tail -- begins with that ramp rather than with steady audio.
    std::vector<float> settle(static_cast<std::size_t>(kRate * 0.25) * kChannels, 0.5F);
    fader.process(settle.data(), settle.size() / kChannels);
    REQUIRE(fader.level() == 1.0);

    const std::size_t span = static_cast<std::size_t>(kRate * 0.2);
    std::vector<float> steady(span * kChannels, 0.5F);
    fader.process(steady.data(), span);

    fader.noteDiscardedFrames(span);
    fader.reset();
    REQUIRE(fader.crossfading());

    std::vector<float> after(span * kChannels, 0.5F);
    fader.process(after.data(), span);

    for (std::size_t frame = 0; frame < span; ++frame) {
        INFO("frame " << frame);
        REQUIRE(after[frame * kChannels] >= Approx(0.5F).margin(0.01));
        REQUIRE(after[frame * kChannels] <= Approx(0.5F * std::sqrt(2.0)).margin(0.01));
    }
    REQUIRE_FALSE(fader.crossfading());
}

TEST_CASE("the old audio audibly decays across a seek", "[dsp]") {
    // Feed a recognisable level, seek into silence: what comes out is the
    // retained tail on its way down. This is the fade *out* -- without it the
    // same sequence emits a hard step to zero.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    std::vector<float> loud(static_cast<std::size_t>(kRate) * kChannels, 0.8F);
    fader.process(loud.data(), loud.size() / kChannels);

    const std::size_t fadeFrames = static_cast<std::size_t>(kRate * 0.2);
    fader.noteDiscardedFrames(fadeFrames);
    fader.reset();
    std::vector<float> silence((fadeFrames + 512) * kChannels, 0.0F);
    fader.process(silence.data(), silence.size() / kChannels);

    // Starts near the old level, decays monotonically, and is gone once the
    // 200 ms tail is spent.
    REQUIRE(silence[0] > 0.75F);
    for (std::size_t frame = 1; frame < fadeFrames; ++frame) {
        REQUIRE(silence[frame * kChannels] <= silence[(frame - 1) * kChannels]);
    }
    for (std::size_t frame = fadeFrames; frame < fadeFrames + 512; ++frame) {
        REQUIRE(silence[frame * kChannels] == 0.0F);
    }
}

TEST_CASE("a second seek cannot resurrect audio from two positions ago", "[dsp]") {
    // reset() clears the history it just captured, so a seek made mid-crossfade
    // fades out at most what was emitted since the previous seek -- not the
    // position before that. Here only ~50 ms elapses between seeks, so the second
    // tail must be exhausted by then, not run the full 200 ms of stale audio.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    std::vector<float> first(static_cast<std::size_t>(kRate) * kChannels, 0.8F);
    fader.process(first.data(), first.size() / kChannels);

    fader.noteDiscardedFrames(static_cast<std::size_t>(kRate * 0.2));
    fader.reset();  // seek one

    const std::size_t betweenFrames = static_cast<std::size_t>(kRate * 0.05);
    std::vector<float> between(betweenFrames * kChannels, 0.0F);
    fader.process(between.data(), betweenFrames);

    // A full fade's worth is claimed to be queued, but only 50 ms has actually
    // been emitted since the last seek, so that is all the tail can be.
    fader.noteDiscardedFrames(static_cast<std::size_t>(kRate * 0.2));
    fader.reset();  // seek two, 50 ms later

    std::vector<float> after(static_cast<std::size_t>(kRate * 0.2) * kChannels, 0.0F);
    fader.process(after.data(), after.size() / kChannels);

    // The second tail is the ~50 ms actually emitted between the seeks; beyond
    // it, silence in must be silence out.
    for (std::size_t frame = betweenFrames + 1; frame < after.size() / kChannels;
         ++frame) {
        REQUIRE(after[frame * kChannels] == 0.0F);
    }
}

TEST_CASE("disabling fading also forgets the history", "[dsp]") {
    // Toggle it off and on, then seek: the tail must be empty, or a seek made
    // minutes later would fade out audio from before the toggle.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    std::vector<float> loud(static_cast<std::size_t>(kRate) * kChannels, 0.8F);
    fader.process(loud.data(), loud.size() / kChannels);

    fader.setEnabled(false);
    fader.setEnabled(true);
    fader.noteDiscardedFrames(static_cast<std::size_t>(kRate * 0.2));
    fader.reset();

    REQUIRE_FALSE(fader.crossfading());
}

// ---------------------------------------------------------------------------
// The upmix routing.
// ---------------------------------------------------------------------------

TEST_CASE("upmix routes by flag, not by position", "[dsp]") {
    // Quad into 5.1: the back pair must land in the back slots (4 and 5 of the
    // 5.1 interleave), leaving centre and LFE silent. Positional copying -- what
    // fitChannels did before -- put them in exactly those two slots, which sent
    // the rear image to the centre speaker and the subwoofer.
    const std::vector<float> quad{0.1F, 0.2F, 0.3F, 0.4F};  // FL FR BL BR
    std::vector<float>       out(6, -1.0F);

    upmix(quad.data(), 4, kConfig4Point0, out.data(), 6, kConfig5Point1, 1);

    REQUIRE(out[0] == 0.1F);  // FL
    REQUIRE(out[1] == 0.2F);  // FR
    REQUIRE(out[2] == 0.0F);  // FC silent
    REQUIRE(out[3] == 0.0F);  // LFE silent
    REQUIRE(out[4] == 0.3F);  // BL
    REQUIRE(out[5] == 0.4F);  // BR
}

TEST_CASE("mono goes to the centre when there is one, the front pair when not", "[dsp]") {
    const std::vector<float> mono{0.7F};

    // 5.1 has a centre: mono lives there and nowhere else.
    std::vector<float> surround(6, -1.0F);
    upmix(mono.data(), 1, kConfigMono, surround.data(), 6, kConfig5Point1, 1);
    REQUIRE(surround[0] == 0.0F);
    REQUIRE(surround[1] == 0.0F);
    REQUIRE(surround[2] == 0.7F);
    REQUIRE(surround[3] == 0.0F);

    // Quad has no centre: the front pair carries it, or it would vanish -- mono
    // *is* the front-centre flag, and plain routing finds it no home.
    std::vector<float> quad(4, -1.0F);
    upmix(mono.data(), 1, kConfigMono, quad.data(), 4, kConfig4Point0, 1);
    REQUIRE(quad[0] == 0.7F);
    REQUIRE(quad[1] == 0.7F);
    REQUIRE(quad[2] == 0.0F);
    REQUIRE(quad[3] == 0.0F);
}

TEST_CASE("6.1 upmixes with its channels in the right places", "[dsp]") {
    // The case Cog gets wrong: its upmix reads the 6.1 side pair at interleave
    // indexes 4-5 and back centre at 6, but flag order puts BC at 4 (bit 8) and
    // the sides at 5-6 (bits 9-10), so Cog rotates three channels. Routing by
    // flag cannot make that mistake; this pins the fix.
    //
    // 6.1 in flag order: FL FR FC LFE BC SL SR.
    const std::vector<float> in{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F};
    // 7.1 in flag order: FL FR FC LFE BL BR SL SR.
    std::vector<float> out(8, -1.0F);

    upmix(in.data(), 7, kConfig6Point1, out.data(), 8, kConfig7Point1, 1);

    REQUIRE(out[0] == 0.1F);  // FL
    REQUIRE(out[1] == 0.2F);  // FR
    REQUIRE(out[2] == 0.3F);  // FC
    REQUIRE(out[3] == 0.4F);  // LFE
    REQUIRE(out[4] == 0.5F);  // BL <- BC split, 7.1 having no BC slot
    REQUIRE(out[5] == 0.5F);  // BR <- BC split
    REQUIRE(out[6] == 0.6F);  // SL, *not* shifted onto the back pair
    REQUIRE(out[7] == 0.7F);  // SR
}

TEST_CASE("a back centre does not clobber a routed back pair", "[dsp]") {
    // An input carrying both a back pair and a back centre: the pair routes to
    // its own slots, and the centre -- having no home and no safe split -- is
    // dropped rather than overwriting them.
    const std::uint32_t inConfig =
        kConfig4Point0 | kChannelBackCenter;  // FL FR BL BR + BC
    const std::vector<float> in{0.1F, 0.2F, 0.3F, 0.4F, 0.9F};
    std::vector<float>       out(8, -1.0F);

    upmix(in.data(), 5, inConfig, out.data(), 8, kConfig7Point1, 1);

    REQUIRE(out[4] == 0.3F);  // BL keeps the routed pair
    REQUIRE(out[5] == 0.4F);  // BR likewise
}

// ---------------------------------------------------------------------------
// Transport fades: pause and stop, which happen at the output rather than in
// the chain, because the audio they fade is already queued for the device.
// ---------------------------------------------------------------------------

TEST_CASE("pause fades out instead of cutting", "[dsp]") {
    RingBuffer ring{1U << 14};
    auto       output = makeOfflineOutput(ring, 8.0);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setEnableFading(true);

    AudioEngine engine{dspRegistry(), *output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(toneWav())));

    for (int spin = 0; spin < 400 && engine.playedSeconds() < 0.4; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(engine.playedSeconds() >= 0.4);

    engine.pause();
    // The status is immediate even though the device stops later; a transport
    // button that waited 200 ms for its own fade would feel broken.
    REQUIRE(engine.status() == PlaybackStatus::Paused);

    // Let the ramp play out, then stop capturing.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    engine.stop();

    const std::vector<float> captured = capturedAudio(*output);
    REQUIRE_FALSE(captured.empty());

    // The tail must arrive at silence by way of a ramp. Sampling the envelope
    // backwards: the last audio is quiet, and it was not quiet 200 ms earlier.
    const auto frames = captured.size() / kChannels;
    const auto window = static_cast<std::size_t>(kRate * 0.02);

    const auto peakOver = [&](std::size_t from, std::size_t count) {
        double peak = 0.0;
        for (std::size_t frame = from; frame < from + count && frame < frames; ++frame) {
            peak = std::max(peak, std::abs(static_cast<double>(
                                      captured[frame * kChannels])));
        }
        return peak;
    };

    // Stopping flushes what the device never played, and the capture records it --
    // as silence, because the fade had already reached zero. So the fade is found
    // relative to the last audible frame rather than to the end of the buffer.
    std::size_t lastAudible = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (std::abs(static_cast<double>(captured[frame * kChannels])) > 0.01) {
            lastAudible = frame;
        }
    }
    REQUIRE(lastAudible > window * 12);

    const double atEnd  = peakOver(lastAudible - window, window);
    const double before = peakOver(lastAudible - (window * 11), window);
    INFO("peak 200 ms before the fade ended " << before << ", at its end " << atEnd);
    REQUIRE(before > 0.2);          // still playing back then
    REQUIRE(atEnd < before / 2.0);  // and well down by the end
}

TEST_CASE("pause cuts immediately when fading is off", "[dsp]") {
    // The setting has to mean something in both directions: with fading off the
    // device stops in pause() itself, with no ramp and no deferred stop.
    RingBuffer ring{1U << 14};
    auto       output = makeOfflineOutput(ring, 8.0);

    auto     store = makeMemorySettingsStore();
    Settings settings{*store};
    settings.setEnableFading(false);

    AudioEngine engine{dspRegistry(), *output, ring, settings};
    REQUIRE(engine.play(Url::fromLocalPath(toneWav())));
    for (int spin = 0; spin < 400 && engine.playedSeconds() < 0.3; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    engine.pause();
    REQUIRE(engine.status() == PlaybackStatus::Paused);
    REQUIRE_FALSE(output->ramping());
    engine.stop();
}

TEST_CASE("the crossfade tail is the discarded audio, not what was already heard",
          "[dsp]") {
    // The bug this pins: taking the tail from the *oldest* end of the history
    // replays audio the listener has already heard -- a stutter. With the
    // discarded count noted, a tail longer than the queue cannot happen, and a
    // shorter queue simply yields a shorter crossfade.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    // Let the track-start fade in land first, or the history holds the ramp
    // multiplied by that fade and every expectation below shifts.
    std::vector<float> settle(static_cast<std::size_t>(kRate * 0.25) * kChannels, 1.0F);
    fader.process(settle.data(), settle.size() / kChannels);
    REQUIRE(fader.level() == 1.0);

    // 200 ms of history: a ramp of distinct values, so which slice is reused is
    // visible rather than a matter of levels.
    const std::size_t historyFrames = static_cast<std::size_t>(kRate * 0.2);
    std::vector<float> feed(historyFrames * kChannels);
    for (std::size_t frame = 0; frame < historyFrames; ++frame) {
        const auto value = static_cast<float>(frame) / static_cast<float>(historyFrames);
        feed[frame * kChannels]       = value;
        feed[(frame * kChannels) + 1] = value;
    }
    fader.process(feed.data(), historyFrames);

    // Only 50 ms is queued, so only the newest 50 ms may be faded out.
    const std::size_t queued = static_cast<std::size_t>(kRate * 0.05);
    fader.noteDiscardedFrames(queued);
    fader.reset();
    REQUIRE(fader.crossfading());

    std::vector<float> after((queued + 256) * kChannels, 0.0F);
    fader.process(after.data(), after.size() / kChannels);

    // The first faded sample must come from near the *end* of that ramp -- the
    // audio that never played -- not from its start.
    // The queued span begins 50 ms from the end of a 200 ms ramp, so the first
    // faded sample is the ramp at 0.75 -- not at 0, which is where the *oldest*
    // end of the history sits.
    const float firstOut = after[0];
    INFO("first tail sample " << firstOut);
    REQUIRE(firstOut == Approx(0.75F).margin(0.01));

    // And the tail is spent after the queued span, not after a full 200 ms.
    for (std::size_t frame = queued + 1; frame < after.size() / kChannels; ++frame) {
        REQUIRE(after[frame * kChannels] == 0.0F);
    }
}

TEST_CASE("the crossfade holds power, not amplitude", "[dsp]") {
    // Constant-power legs, which is the departure from Cog's linear ramp: a seek
    // lands on uncorrelated audio, so the two sides add in power. At the midpoint
    // each leg must sit near 1/sqrt(2), giving squares that sum to one -- a linear
    // ramp would put both at 0.5 and lose 3 dB.
    Fader fader;
    fader.setEnabled(true);
    fader.prepare(formatFor(kRate, kChannels));

    std::vector<float> settle(static_cast<std::size_t>(kRate * 0.25) * kChannels, 1.0F);
    fader.process(settle.data(), settle.size() / kChannels);
    REQUIRE(fader.level() == 1.0);

    const std::size_t span = static_cast<std::size_t>(kRate * 0.1);
    std::vector<float> feed(span * kChannels, 1.0F);
    fader.process(feed.data(), span);

    fader.noteDiscardedFrames(span);
    fader.reset();

    // Incoming silence isolates the outgoing leg: what comes out *is* cos(theta).
    std::vector<float> after(span * kChannels, 0.0F);
    fader.process(after.data(), span);

    const std::size_t middle = span / 2;
    const double      leg    = std::abs(static_cast<double>(after[middle * kChannels]));
    INFO("outgoing leg at the midpoint " << leg);
    REQUIRE(leg == Approx(1.0 / std::sqrt(2.0)).margin(0.03));
}
