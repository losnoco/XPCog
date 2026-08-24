// ReplayGain, settings and the resampler.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioConverter.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"

#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace xpcog;

// --- ReplayGain -----------------------------------------------------------

namespace {

ReplayGainInfo gainInfo() {
    ReplayGainInfo info;
    info.trackGain = -6.0F;
    info.trackPeak = 0.9F;
    info.albumGain = -3.0F;
    info.albumPeak = 0.95F;
    return info;
}

}  // namespace

TEST_CASE("replayGainScale converts dB to linear gain", "[replaygain]") {
    CHECK(dbToScale(0.0F) == Catch::Approx(1.0F));
    CHECK(dbToScale(-6.0F) == Catch::Approx(0.501187F).epsilon(0.001));
    CHECK(dbToScale(6.0F) == Catch::Approx(1.995262F).epsilon(0.001));
}

TEST_CASE("replayGainScale picks the requested tier", "[replaygain]") {
    const ReplayGainInfo info = gainInfo();

    CHECK(replayGainScale(info, VolumeScaling::kAlbumGain) ==
          Catch::Approx(dbToScale(-3.0F)));
    CHECK(replayGainScale(info, VolumeScaling::kTrackGain) ==
          Catch::Approx(dbToScale(-6.0F)));
    CHECK(replayGainScale(info, VolumeScaling::kNone) == Catch::Approx(1.0F));
    CHECK(replayGainScale(info, "") == Catch::Approx(1.0F));
}

TEST_CASE("album mode falls back to track gain when no album gain exists",
          "[replaygain]") {
    // The tiers cascade, and that cascade IS the fallback: asking for album gain
    // on a file that only carries track gain must still be scaled, not ignored.
    ReplayGainInfo info;
    info.trackGain = -6.0F;

    CHECK(replayGainScale(info, VolumeScaling::kAlbumGain) ==
          Catch::Approx(dbToScale(-6.0F)));
}

TEST_CASE("no gain information leaves the audio untouched", "[replaygain]") {
    const ReplayGainInfo empty;
    CHECK(replayGainScale(empty, VolumeScaling::kAlbumGainWithPeak) ==
          Catch::Approx(1.0F));
}

TEST_CASE("WithPeak pulls the gain back so it cannot clip", "[replaygain]") {
    // A positive gain that would push the loudest sample past full scale must be
    // clamped to exactly 1/peak.
    ReplayGainInfo info;
    info.albumGain = +6.0F;
    info.albumPeak = 0.8F;

    const float plain = replayGainScale(info, VolumeScaling::kAlbumGain);
    const float capped = replayGainScale(info, VolumeScaling::kAlbumGainWithPeak);

    CHECK(plain == Catch::Approx(dbToScale(6.0F)));
    CHECK(capped == Catch::Approx(1.0F / 0.8F));
    CHECK(capped < plain);
    // The whole point: scaled peak now sits exactly at full scale.
    CHECK(capped * 0.8F == Catch::Approx(1.0F));
}

TEST_CASE("WithPeak leaves a gain that already fits alone", "[replaygain]") {
    ReplayGainInfo info;
    info.albumGain = -6.0F;
    info.albumPeak = 0.9F;

    CHECK(replayGainScale(info, VolumeScaling::kAlbumGainWithPeak) ==
          Catch::Approx(dbToScale(-6.0F)));
}

// --- Settings -------------------------------------------------------------

TEST_CASE("settings return their defaults when unset", "[settings]") {
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    CHECK(settings.VolumeScaling() == "albumGainWithPeak");
    CHECK(settings.EnableFading());
    CHECK(settings.Volume() == Catch::Approx(1.0));
    CHECK(settings.SettingsSchemaVersion() == 0);

    // Pinned because the failure is silent in the wrong direction: a default of
    // true means the "still playing, in the tray" notice is never shown to
    // anyone, and nothing about that looks broken.
    CHECK_FALSE(settings.TrayHideAnnounced());

    // 0 is Cog's rule -- selection, falling back to the playing track. The other
    // mode is a deliberate departure from Cog, so it must be the one you choose
    // rather than the one you get.
    CHECK(settings.PanelFollowMode() == 0);

    // Cog's defaults, and the only two in this file that default to *on*. Both
    // are Cog's call rather than a new one, so an installation carrying a Cog
    // plist over keeps behaving the way it did.
    CHECK(settings.NotificationsEnable());
    CHECK(settings.NotificationsShowAlbumArt());

    // The keys keep Cog's dots. Pinned because a well-meaning tidy to
    // `notificationsEnable` would compile, pass every other test, and silently
    // stop reading the value an imported Cog plist actually carries.
    CHECK(Settings::defaultValue("notifications.enable") == "true");
    CHECK(Settings::defaultValue("notifications.show-album-art") == "true");
}

TEST_CASE("crash reporting is off until it is consented to", "[settings]") {
    // The one default in this file that is a promise to the listener rather than
    // a preference, so it is pinned on its own. A true here -- from a typo, or
    // from someone assuming the prompt is what gates it -- means an untouched
    // installation starts reporting on its first launch, before anyone has been
    // asked anything, and nothing about that is visible from the interface.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    CHECK_FALSE(settings.SentryConsented());

    // And "not asked yet" must be distinguishable from "asked and declined",
    // because the prompt is shown exactly once and this is what decides whether
    // it has been.
    CHECK_FALSE(settings.SentryAskedConsent());

    // Cog's keys, unchanged, so a preferences file that has come from Cog on
    // macOS carries the answer over rather than asking again -- and, in the
    // direction that matters more, so that someone who declined in Cog is not
    // asked afresh here.
    CHECK(Settings::defaultValue("sentryConsented") == "false");
    CHECK(Settings::defaultValue("sentryAskedConsent") == "false");
}

TEST_CASE("settings round-trip through the store", "[settings]") {
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    settings.setVolumeScaling("trackGain");
    settings.setEnableFading(false);
    settings.setVolume(0.25);

    CHECK(settings.VolumeScaling() == "trackGain");
    CHECK_FALSE(settings.EnableFading());
    CHECK(settings.Volume() == Catch::Approx(0.25));
}

TEST_CASE("settings accept Cog's plist boolean spellings", "[settings]") {
    // An imported Cog preferences file writes YES/NO; misreading those would
    // silently reset the user's configuration to defaults.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    store->setRaw("enableFading", "NO");
    CHECK_FALSE(settings.EnableFading());

    store->setRaw("enableFading", "YES");
    CHECK(settings.EnableFading());
}

TEST_CASE("a malformed value falls back rather than throwing", "[settings]") {
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    store->setRaw("volume", "not a number");
    CHECK(settings.Volume() == Catch::Approx(1.0));
}

TEST_CASE("Settings::all describes every setting", "[settings]") {
    const auto all = Settings::all();
    REQUIRE_FALSE(all.empty());

    bool foundVolumeScaling = false;
    for (const auto& descriptor : all) {
        CHECK_FALSE(descriptor.key.empty());
        CHECK_FALSE(descriptor.ident.empty());
        if (descriptor.key == "volumeScaling") {
            foundVolumeScaling = true;
        }
    }
    CHECK(foundVolumeScaling);
}

TEST_CASE("resetAll restores defaults", "[settings]") {
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    settings.setVolumeScaling("none");
    REQUIRE(settings.VolumeScaling() == "none");

    settings.resetAll();
    CHECK(settings.VolumeScaling() == "albumGainWithPeak");
}

TEST_CASE("migrations run once and record the version", "[settings]") {
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    // Cog stored volume as a percentage.
    store->setRaw("volume", "75");
    settings.applyMigrations();

    CHECK(settings.Volume() == Catch::Approx(0.75));
    CHECK(settings.SettingsSchemaVersion() >= 1);

    // Running again must not divide a second time.
    settings.applyMigrations();
    CHECK(settings.Volume() == Catch::Approx(0.75));
}

TEST_CASE("an existing curve survives the equaliser gaining a switch",
          "[settings]") {
    // The upgrade hazard, and the reason this migration exists. GraphicEQenable
    // defaults to off -- Cog's default, for a setting that is Cog's -- which is
    // right for a new install and silently wrong for every settings file written
    // before the switch existed, because until then a non-flat curve was simply
    // always on. Without this, upgrading would have turned everybody's equaliser
    // off and left the sliders showing a curve nobody could hear.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    // A settings file from before the switch: a curve, and no opinion about
    // whether the equaliser runs.
    store->setRaw("eq1kHz", "6.0");
    REQUIRE_FALSE(store->getRaw("GraphicEQenable").has_value());

    settings.applyMigrations();

    CHECK(settings.GraphicEqEnable());
    CHECK(settings.SettingsSchemaVersion() >= 2);
}

TEST_CASE("a flat curve does not switch the equaliser on", "[settings]") {
    // Flat is skipped whether or not it is enabled, so turning the switch on
    // would change nothing audible and would leave a checkbox ticked that the
    // user never touched. Doing nothing is the honest answer.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    store->setRaw("eq1kHz", "0.0");
    store->setRaw("eqPreamp", "0.0");

    settings.applyMigrations();

    CHECK_FALSE(settings.GraphicEqEnable());
}

TEST_CASE("the preamp alone counts as a curve", "[settings]") {
    // The case a check written as "are all 31 bands zero" would miss: somebody
    // whose only equaliser setting is a preamp was hearing it, and must go on
    // hearing it.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    store->setRaw("eqPreamp", "-4.0");
    settings.applyMigrations();

    CHECK(settings.GraphicEqEnable());
}

TEST_CASE("a stated preference about the switch is not overridden",
          "[settings]") {
    // Somebody who has turned it off, or a settings file that has been through a
    // Cog import carrying GraphicEQenable=false, has already said what they
    // want. A migration exists to supply an answer where there is none, not to
    // replace one.
    auto     store = makeMemorySettingsStore();
    Settings settings(*store);

    store->setRaw("eq1kHz", "6.0");
    store->setRaw("GraphicEQenable", "false");

    settings.applyMigrations();

    CHECK_FALSE(settings.GraphicEqEnable());
}

// --- AudioConverter -------------------------------------------------------

namespace {

AudioChunk toneChunk(double rate, std::uint32_t channels, std::size_t frames,
                     double freq = 440.0) {
    AudioFormat format;
    format.sampleRate    = rate;
    format.channels      = channels;
    format.format        = SampleFormat::F32;
    format.bitsPerSample = 32;
    format.channelConfig = guessChannelConfig(channels);

    AudioChunk chunk;
    chunk.setFormat(format);
    auto* samples = reinterpret_cast<float*>(chunk.allocFrames(frames));

    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(
            0.5 * std::sin(xpcog::test::kTwoPi * freq * (static_cast<double>(f) / rate)));
        for (std::uint32_t c = 0; c < channels; ++c) {
            samples[f * channels + c] = v;
        }
    }
    return chunk;
}

}  // namespace

TEST_CASE("matching rates pass through bit-exactly", "[converter]") {
    // The resampler null test: no rate change must mean no arithmetic at all, or
    // every same-rate file is subtly altered for no reason.
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(44100.0, 2));

    const AudioChunk chunk = toneChunk(44100.0, 2, 1024);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));
    REQUIRE(out.size() == 1024 * 2);

    const auto* input = reinterpret_cast<const float*>(chunk.bytes().data());
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == input[i]);
    }
}

TEST_CASE("a rate change produces the expected number of frames", "[converter]") {
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    const AudioChunk chunk = toneChunk(44100.0, 2, 44100);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));
    converter.drain(out);

    // One second in, one second out, within the resampler's edge tolerance.
    const double seconds = static_cast<double>(out.size() / 2) / 48000.0;
    CHECK(seconds == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("resampling preserves the tone", "[converter]") {
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    const AudioChunk chunk = toneChunk(44100.0, 2, 44100, 440.0);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));
    converter.drain(out);
    REQUIRE(out.size() > 48000);

    // A resampler that mangles the signal would shift its pitch; count zero
    // crossings over a steady-state window to confirm it did not.
    const std::size_t frames = out.size() / 2;
    const std::size_t begin  = frames / 4;
    const std::size_t end    = frames * 3 / 4;

    std::size_t crossings = 0;
    float       previous  = 0.0F;
    for (std::size_t i = begin; i < end; ++i) {
        const float v = out[i * 2];
        if (i > begin && ((previous < 0.0F) != (v < 0.0F))) {
            ++crossings;
        }
        previous = v;
    }
    const double freq = static_cast<double>(crossings) * 48000.0 /
                        (2.0 * static_cast<double>(end - begin));
    CHECK(freq == Catch::Approx(440.0).margin(5.0));
}

TEST_CASE("the converter resamples again after a drain", "[converter]") {
    // The track seam: the engine drains at the handoff so the outgoing track's
    // tail is not left in the delay line, and then feeds the incoming track
    // through the same converter. A drained soxr instance cannot be fed again,
    // so this crashed whenever both tracks shared a rate -- the case where
    // configureFor() sees nothing to rebuild.
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    const AudioChunk chunk = toneChunk(44100.0, 2, 44100, 440.0);

    std::vector<float> first;
    REQUIRE(converter.process(chunk, first));
    converter.drain(first);
    REQUIRE(first.size() > 48000);

    // Same rate and channel count as the outgoing track, which is what makes
    // this the regression rather than an ordinary reconfiguration.
    std::vector<float> second;
    REQUIRE(converter.process(chunk, second));
    converter.drain(second);

    CHECK(second.size() == first.size());
    // Not just the same length: a fresh instance starts from the same primed
    // state, so the second pass is the first one over again.
    for (std::size_t i = 0; i < second.size(); ++i) {
        REQUIRE(second[i] == first[i]);
    }
}

namespace {

/// The tone toneChunk() would have produced had it been generated at `rate`
/// directly, which is what a correct resampling of it should look like.
[[nodiscard]] double idealTone(std::size_t frame, double rate, double freq = 440.0) {
    return 0.5 * std::sin(xpcog::test::kTwoPi * freq * (static_cast<double>(frame) / rate));
}

/// Worst deviation from that tone over a window of the left channel.
[[nodiscard]] double worstError(const std::vector<float>& out, std::size_t from,
                                std::size_t to, double rate, std::size_t offset = 0) {
    double worst = 0.0;
    for (std::size_t f = from; f < to; ++f) {
        const double error =
            std::abs(static_cast<double>(out[f * 2]) - idealTone(f - offset, rate));
        worst = std::max(worst, error);
    }
    return worst;
}

}  // namespace

TEST_CASE("the resampler's edges are as clean as its middle", "[converter]") {
    // What the LPC extrapolation buys. The resampler's filter is centred on the
    // sample it is producing and reaches some way either side, so at the first
    // and last block it convolves real signal against the implicit silence
    // outside the data -- a step, which is a click, and which is audible at
    // every track start and every seam. Predicting a continuation gives it
    // something to reach into instead.
    //
    // Measured against the tone the chunk would have been had it been generated
    // at 48 kHz in the first place: the edges must be as accurate as the steady
    // state, not merely close to it. Before the extrapolation the edge error was
    // 8.7e-04 against a steady state of 2.4e-07 -- three orders of magnitude,
    // which is the click.
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    std::vector<float> out;
    REQUIRE(converter.process(toneChunk(44100.0, 2, 44100, 440.0), out));
    converter.drain(out);

    const std::size_t frames = out.size() / 2;
    REQUIRE(frames > 40000);

    const double middle = worstError(out, 20000, 20400, 48000.0);
    const double head   = worstError(out, 0, 400, 48000.0);
    const double tail   = worstError(out, frames - 400, frames, 48000.0);

    // Generous against the steady state -- the point is the order of magnitude,
    // and a different soxr build may place its filter slightly differently.
    CHECK(head < middle * 8.0);
    CHECK(tail < middle * 8.0);
    CHECK(head < 1.0e-5);
    CHECK(tail < 1.0e-5);
}

TEST_CASE("the padding does not change how many frames come out", "[converter]") {
    // The extrapolation feeds the resampler 2205 frames it never asked for at
    // each end, and takes 2400 output frames back off for each. Those two
    // numbers are the input:output ratio reduced by its GCD, so the trim is
    // exact rather than rounded -- one second in still means one second out, to
    // the frame. Round the pair independently and the leftover fraction is what
    // stops a seam landing where it should.
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    std::vector<float> out;
    REQUIRE(converter.process(toneChunk(44100.0, 2, 44100), out));
    converter.drain(out);

    CHECK(out.size() / 2 == 48000);
}

TEST_CASE("a second track's edges are extrapolated too", "[converter]") {
    // The seam. drain() disposes of the resampler, so the incoming track gets a
    // fresh one with no history -- which means it needs its run-up predicted
    // again, and the trim that takes the run-up back off re-armed with it.
    // Getting that wrong is silent: the audio is all there, one click per track.
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));

    std::vector<float> first;
    REQUIRE(converter.process(toneChunk(44100.0, 2, 44100, 440.0), first));
    converter.drain(first);

    std::vector<float> second;
    REQUIRE(converter.process(toneChunk(44100.0, 2, 44100, 440.0), second));
    converter.drain(second);

    REQUIRE(second.size() == first.size());
    const std::size_t frames = second.size() / 2;

    const double middle = worstError(second, 20000, 20400, 48000.0);
    CHECK(worstError(second, 0, 400, 48000.0) < middle * 8.0);
    CHECK(worstError(second, frames - 400, frames, 48000.0) < middle * 8.0);
}

TEST_CASE("gain is applied on the way through", "[converter]") {
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(44100.0, 2));
    converter.setGain(0.5F);

    const AudioChunk chunk = toneChunk(44100.0, 2, 512);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));

    const auto* input = reinterpret_cast<const float*>(chunk.bytes().data());
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == Catch::Approx(input[i] * 0.5F));
    }
}

TEST_CASE("HDCD decoding is transparent for ordinary CD audio", "[converter][hdcd]") {
    // HDCD runs on every 16-bit 44.1 kHz stereo lossless stream, which is almost
    // all CD-sourced material and almost none of it actually HDCD. It must
    // therefore be bit-transparent when no codes are present, or enabling it
    // quietly alters every album the user owns.
    AudioFormat format;
    format.sampleRate    = 44100.0;
    format.channels      = 2;
    format.format        = SampleFormat::S16;
    format.bitsPerSample = 16;
    format.channelConfig = guessChannelConfig(2);

    AudioChunk chunk;
    chunk.setFormat(format);
    chunk.lossless = true;
    auto* samples  = reinterpret_cast<std::int16_t*>(chunk.allocFrames(2048));
    for (std::size_t f = 0; f < 2048; ++f) {
        const auto v = static_cast<std::int16_t>(
            20000.0 * std::sin(xpcog::test::kTwoPi * 440.0 * (static_cast<double>(f) / 44100.0)));
        samples[f * 2]     = v;
        samples[f * 2 + 1] = static_cast<std::int16_t>(-v);
    }

    std::vector<float> withHdcd;
    {
        AudioConverter converter;
        REQUIRE(converter.setOutputFormat(44100.0, 2));
        converter.setHdcdEnabled(true);
        REQUIRE(converter.process(chunk, withHdcd));
        // Plain audio carries no codes, so nothing should be reported.
        CHECK_FALSE(converter.hdcdDetected());
    }

    std::vector<float> withoutHdcd;
    {
        AudioConverter converter;
        REQUIRE(converter.setOutputFormat(44100.0, 2));
        converter.setHdcdEnabled(false);
        REQUIRE(converter.process(chunk, withoutHdcd));
    }

    REQUIRE(withHdcd.size() == withoutHdcd.size());
    for (std::size_t i = 0; i < withHdcd.size(); ++i) {
        REQUIRE(withHdcd[i] == withoutHdcd[i]);
    }
}

TEST_CASE("HDCD is not attempted on formats that cannot carry it",
          "[converter][hdcd]") {
    // 48 kHz is not a Red Book rate, so the codes cannot be there.
    AudioFormat format;
    format.sampleRate    = 48000.0;
    format.channels      = 2;
    format.format        = SampleFormat::S16;
    format.bitsPerSample = 16;
    format.channelConfig = guessChannelConfig(2);

    AudioChunk chunk;
    chunk.setFormat(format);
    chunk.lossless = true;
    auto* samples  = reinterpret_cast<std::int16_t*>(chunk.allocFrames(512));
    for (std::size_t i = 0; i < 1024; ++i) {
        samples[i] = static_cast<std::int16_t>((i % 200) * 100 - 10000);
    }

    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(48000.0, 2));
    converter.setHdcdEnabled(true);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));
    CHECK_FALSE(converter.hdcdDetected());

    // Values must still be the plain 16-bit conversion.
    CHECK(out[0] == Catch::Approx(static_cast<float>(samples[0]) / 32768.0F));
}

TEST_CASE("mono is spread across every output channel", "[converter]") {
    AudioConverter converter;
    REQUIRE(converter.setOutputFormat(44100.0, 2));

    const AudioChunk chunk = toneChunk(44100.0, 1, 256);

    std::vector<float> out;
    REQUIRE(converter.process(chunk, out));
    REQUIRE(out.size() == 256 * 2);

    // Silent on one side would be the obvious bug here.
    for (std::size_t f = 0; f < 256; ++f) {
        REQUIRE(out[f * 2] == out[f * 2 + 1]);
    }
}
