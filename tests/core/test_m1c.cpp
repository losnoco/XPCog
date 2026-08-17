// ReplayGain, settings and the resampler.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioConverter.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"

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
            0.5 * std::sin(2.0 * M_PI * freq * (static_cast<double>(f) / rate)));
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
            20000.0 * std::sin(2.0 * M_PI * 440.0 * (static_cast<double>(f) / 44100.0)));
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
