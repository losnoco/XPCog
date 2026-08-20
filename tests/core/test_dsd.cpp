// DSD on its way to PCM.
//
// The M6 half of `SampleFormat::DSD`, which existed as an enumerator and a
// `return 0` in the sample conversion until the decimation filter arrived.
//
// A one-bit stream is testable in a way most audio is not: the encoding of
// silence and of full scale are both exact bit patterns, so what the filter
// should produce for them is a number rather than a judgement.
//
//   0xAA repeated is DSD's zero -- a bit stream that flips every sample
//   averages to nothing. Repeated is the operative word, and the first version
//   of this file got it wrong: alternating 0x55 with 0xAA keeps the alternation
//   inside each byte and breaks it at every boundary, which is a tone at one
//   sixteenth of the byte rate -- 44.1 kHz, outside the filter's passband but
//   well inside its transition band, and it came through at 0.049. A single
//   repeated byte alternates across the boundaries too.
//
//   Anything other than zero here would mean the filter's history was primed
//   wrong, which is the mistake of filling it with zero bytes: a zero byte is
//   eight negative samples, so a stream would open with a step to negative full
//   scale and ring for 64 taps.
//
//   0xFF is every sample positive, and the filter's stated gain is 2.0, so it
//   settles at +2.0 rather than +1.0. That is deliberate and is why there is a
//   setting to halve it: DSD's practical ceiling is about half modulation, so
//   the gain puts a hot recording at full scale instead of 6 dB under.
//
// The end-to-end case wants a real DSD file and skips without one, the same
// shape the corpus tests use. What it can prove that the synthetic cases cannot
// is that a decoder actually reports the format -- the conversion could be
// perfect and never be reached.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/AudioConverter.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/Settings.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <string_view>
#include <vector>

using Catch::Approx;

namespace {

namespace fs = std::filesystem;

/// DSD128 as WavPack reports it: one frame is one byte per channel, so the rate
/// is the bit rate over eight and the duration arithmetic stays ordinary.
constexpr double kDsdRate = 705600.0;

[[nodiscard]] xpcog::AudioChunk dsdChunk(std::uint8_t pattern, std::size_t frames) {
    xpcog::AudioFormat format;
    format.sampleRate    = kDsdRate;
    format.channels      = 2;
    format.channelConfig = 0x3;
    format.format        = xpcog::SampleFormat::DSD;
    format.bitsPerSample = 1;

    xpcog::AudioChunk chunk;
    chunk.setFormat(format);
    chunk.lossless = true;

    std::byte* bytes = chunk.allocFrames(frames);
    for (std::size_t i = 0; i < frames * 2; ++i) {
        bytes[i] = static_cast<std::byte>(pattern);
    }
    chunk.setFrameCount(frames);
    return chunk;
}

/// The mean of everything after `skip` frames, which is where the filter has
/// finished swallowing its own priming.
[[nodiscard]] double settledMean(const std::vector<float>& out, std::size_t skip) {
    if (out.size() <= skip * 2) {
        return 0.0;
    }
    double      sum   = 0.0;
    std::size_t count = 0;
    for (std::size_t i = skip * 2; i < out.size(); ++i) {
        sum += static_cast<double>(out[i]);
        ++count;
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

[[nodiscard]] float peakOf(const std::vector<float>& out) {
    float highest = 0.0F;
    for (const float sample : out) {
        highest = std::max(highest, std::fabs(sample));
    }
    return highest;
}

#ifdef XPCOG_DSD_FILE
constexpr bool kHaveDsdFile = true;
[[nodiscard]] fs::path dsdFile() { return fs::path{XPCOG_DSD_FILE}; }
#else
constexpr bool kHaveDsdFile = false;
[[nodiscard]] fs::path dsdFile() { return {}; }
#endif


// ---------------------------------------------------------------------------
// A decoder that counts frames faster than the device does
// ---------------------------------------------------------------------------
// Everything below could be done with a real DSD file, and one case does that.
// This one does not need it, and it is sharper: what is asserted is the frame
// number the engine *asks the decoder for*, which is the number that was wrong.

std::atomic<std::int64_t> g_seekRequest{-1};

class FakeDsdDecoder final : public xpcog::IDecoder {
public:
    bool open(xpcog::ISource* /*source*/) override { return true; }

    [[nodiscard]] xpcog::TrackProperties properties() const override {
        xpcog::TrackProperties props;
        props.format.sampleRate    = kDsdRate;
        props.format.channels      = 2;
        props.format.channelConfig = 0x3;
        props.format.format        = xpcog::SampleFormat::DSD;
        props.format.bitsPerSample = 1;
        // Ten minutes of it, so a seek to a minute in is well inside.
        props.totalFrames = static_cast<std::int64_t>(kDsdRate * 600.0);
        props.seekable    = true;
        props.codec       = "FakeDSD";
        return props;
    }

    bool readAudio(xpcog::AudioChunk& out) override {
        constexpr std::size_t kFrames = 4096;
        out.clear();
        out.setFormat(properties().format);
        std::byte* bytes = out.allocFrames(kFrames);
        // DSD silence, so nothing here can be mistaken for a signal.
        std::fill_n(bytes, kFrames * 2, std::byte{0xAA});
        out.setFrameCount(kFrames);
        position_ += static_cast<std::int64_t>(kFrames);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        g_seekRequest.store(frame, std::memory_order_release);
        position_ = frame;
        return frame;
    }

    void close() override {}

private:
    std::int64_t position_ = 0;
};

constexpr std::string_view kFakeExtensions[] = {"fakedsd"};

/// A file for the fake decoder to be handed. Its contents do not matter -- the
/// decoder ignores the source -- but the registry opens it, so it has to exist.
[[nodiscard]] fs::path fakeTrackFile() {
    const fs::path dir = fs::temp_directory_path() / "xpcog-dsd-tests";
    fs::create_directories(dir);
    const fs::path path = dir / "fake.fakedsd";
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputc(0, file);
        std::fclose(file);
    }
    return path;
}

}  // namespace

TEST_CASE("DSD silence decodes to silence", "[audio][dsd]") {
    xpcog::AudioConverter converter;
    // Rendered at its own rate, so the resampler is out of the picture and what
    // is measured is the filter alone.
    REQUIRE(converter.setOutputFormat(kDsdRate, 2, "high"));

    std::vector<float> out;
    REQUIRE(converter.process(dsdChunk(0xAA, 4096), out));
    REQUIRE(out.size() == 4096 * 2);

    // The exact number matters more than it looks: filling the filter's history
    // with zero bytes instead of 0x55/0xAA would put a large negative
    // excursion at the start of every stream and every seek.
    CHECK(settledMean(out, 128) == Approx(0.0).margin(1e-6));
    CHECK(peakOf(out) < 0.01F);
}

TEST_CASE("DSD full scale decodes to the filter's stated gain", "[audio][dsd]") {
    xpcog::AudioConverter converter;
    REQUIRE(converter.setOutputFormat(kDsdRate, 2, "high"));

    std::vector<float> out;
    REQUIRE(converter.process(dsdChunk(0xFF, 4096), out));
    REQUIRE(out.size() == 4096 * 2);

    CHECK(settledMean(out, 128) == Approx(2.0).margin(0.01));
}

TEST_CASE("halving DSD brings full modulation back to full scale",
          "[audio][dsd]") {
    xpcog::AudioConverter converter;
    REQUIRE(converter.setOutputFormat(kDsdRate, 2, "high"));
    converter.setHalveDsd(true);

    std::vector<float> out;
    REQUIRE(converter.process(dsdChunk(0xFF, 4096), out));
    CHECK(settledMean(out, 128) == Approx(1.0).margin(0.01));
}

TEST_CASE("DSD is decimated eight to one and then resampled like anything else",
          "[audio][dsd]") {
    xpcog::AudioConverter converter;
    REQUIRE(converter.setOutputFormat(44100.0, 2, "high"));

    std::vector<float> out;
    REQUIRE(converter.process(dsdChunk(0xAA, 70560), out));
    converter.drain(out);

    // 70,560 bytes per channel is a tenth of a second of DSD128, and a tenth of
    // a second at 44,100 is 4,410 frames. The count is what proves the byte
    // count is being read as frames rather than as samples: a factor of eight
    // wrong either way lands nowhere near this.
    const auto frames = static_cast<double>(out.size() / 2);
    CHECK(frames == Approx(4410.0).margin(64.0));
}

TEST_CASE("a seek does not carry the old position's history across",
          "[audio][dsd]") {
    xpcog::AudioConverter converter;
    REQUIRE(converter.setOutputFormat(kDsdRate, 2, "high"));

    std::vector<float> loud;
    REQUIRE(converter.process(dsdChunk(0xFF, 1024), loud));

    // Reset, then silence. Without the filters being reset the 64 taps still
    // hold full-scale ones, and the first samples after the seek ring.
    converter.reset();

    std::vector<float> quiet;
    REQUIRE(converter.process(dsdChunk(0xAA, 1024), quiet));
    REQUIRE_FALSE(quiet.empty());
    CHECK(peakOf(quiet) < 0.01F);
}

TEST_CASE("a DSD file reaches the converter as DSD", "[audio][dsd]") {
    if (!kHaveDsdFile || !fs::exists(dsdFile())) {
        SKIP("no DSD file: configure with -DXPCOG_DSD_FILE=<path to a DSD .wv>");
    }

    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    xpcog::PluginRegistry::OpenResult opened =
        registry.open(xpcog::Url::fromLocalPath(dsdFile()));
    REQUIRE(opened);

    const xpcog::TrackProperties props = opened.decoder->properties();
    INFO("rate " << props.format.sampleRate);
    // The decoder's part: without this the bytes are handed on as if they were
    // PCM samples, which is what a DSD file sounded like before M6.
    CHECK(props.format.format == xpcog::SampleFormat::DSD);
    CHECK(props.format.bitsPerSample == 1);
    // The byte rate, which is the native rate over eight. DSD is named by its
    // multiple of 44,100: DSD64 is 2.8224 MHz and each step up doubles it, so
    // the byte rates are 352,800, 705,600, 1,411,200 and 2,822,400.
    //
    // Written as the family rather than as a list of two, which is what it was
    // until a DSD256 rip was pointed at it -- the assertion had never run,
    // because it needs a fixture nobody had. Anything outside the family means
    // the native bit rate leaked in, which is the failure worth catching.
    const double byteRate = props.format.sampleRate;
    bool         isDsdRate = false;
    for (double rate = 352800.0; rate <= 2822400.0; rate *= 2.0) {
        isDsdRate = isDsdRate || byteRate == rate;
    }
    CHECK(isDsdRate);

    xpcog::AudioConverter converter;
    REQUIRE(converter.setOutputFormat(44100.0, props.format.channels, "high"));

    std::vector<float> out;
    xpcog::AudioChunk  chunk;
    // A second or so of it, which for an SACD rip is well past any lead-in.
    while (out.size() < 44100 * 2 && opened.decoder->readAudio(chunk)) {
        REQUIRE(converter.process(chunk, out));
    }

    REQUIRE_FALSE(out.empty());
    // Music, not silence and not the noise a raw one-bit stream makes when it
    // is played as PCM -- which would sit near full scale everywhere.
    const float peak = peakOf(out);
    INFO("peak " << peak);
    CHECK(peak > 0.001F);
    CHECK(peak < 4.0F);
}

TEST_CASE("a DSD track actually plays", "[audio][dsd]") {
    if (!kHaveDsdFile || !fs::exists(dsdFile())) {
        SKIP("no DSD file: configure with -DXPCOG_DSD_FILE=<path to a DSD .wv>");
    }

    // The case the conversion tests above could not fail on, and the one that
    // was wrong: everything from the decoder to the filter was right, and the
    // track still would not play, because the engine ran the device at the
    // track's own rate -- 705,600 Hz, which no backend will open. play()
    // returned false and nothing said why.
    //
    // Note what this can and cannot prove. The offline output takes any rate at
    // all, so it cannot refuse the way miniaudio does; what is asserted instead
    // is the rate the engine *chose*, which is where the decision lives.
    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);

    xpcog::RingBuffer ring(48000 * 2);
    auto              output = xpcog::makeOfflineOutput(ring);
    auto              store  = xpcog::makeMemorySettingsStore();
    xpcog::Settings   settings(*store);
    xpcog::AudioEngine engine(registry, *output, ring, settings);

    REQUIRE(engine.play(xpcog::Url::fromLocalPath(dsdFile())));

    // miniaudio refuses anything above 384,000 (miniaudio.h:126), so this is
    // the assertion that fails on the bug rather than on its symptom.
    const double deviceRate = output->negotiatedFormat().sampleRate;
    INFO("device rate " << deviceRate);
    CHECK(deviceRate <= 384000.0);
    CHECK(deviceRate > 0.0);

    // A second of it is plenty; this is a twenty-minute side.
    for (int spin = 0; spin < 200 && xpcog::capturedAudio(*output).size() < 44100 * 2;
         ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::vector<float> played = xpcog::capturedAudio(*output);
    engine.stop();

    REQUIRE_FALSE(played.empty());
    const float peak = peakOf(played);
    INFO("peak " << peak << " over " << played.size() << " samples");
    CHECK(peak > 0.001F);
    CHECK(peak < 4.0F);
}

TEST_CASE("a seek asks the decoder for the decoder's own frames",
          "[audio][dsd][seek]") {
    // Before registerAllCodecs, which ends in freeze() -- adding a decoder after
    // that asserts, and an assert in a test is a dialog box on somebody's screen.
    xpcog::PluginRegistry registry;
    registry.addDecoder(xpcog::DecoderDescriptor{
        .name       = "FakeDsdDecoder",
        .extensions = kFakeExtensions,
        .create     = [] { return xpcog::DecoderPtr{new FakeDsdDecoder()}; },
    });
    xpcog::registerAllCodecs(registry);

    const fs::path path = fakeTrackFile();

    xpcog::RingBuffer  ring(48000 * 2);
    auto               output = xpcog::makeOfflineOutput(ring);
    auto               store  = xpcog::makeMemorySettingsStore();
    xpcog::Settings    settings(*store);
    xpcog::AudioEngine engine(registry, *output, ring, settings);

    g_seekRequest.store(-1, std::memory_order_release);
    REQUIRE(engine.play(xpcog::Url::fromLocalPath(path)));

    // The device is not running at the track's rate and cannot: 705,600 Hz is
    // past what any backend accepts, so the engine fell back. That gap is
    // exactly what a seek has to cross.
    REQUIRE(output->negotiatedFormat().sampleRate != kDsdRate);

    REQUIRE(engine.seek(60.0));
    for (int spin = 0; spin < 400 && g_seekRequest.load(std::memory_order_acquire) < 0;
         ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::int64_t asked = g_seekRequest.load(std::memory_order_acquire);
    engine.stop();

    // A minute in, counted the way the decoder counts. Measured against the
    // device's rate instead, this asked for 2,880,000 -- four seconds of music
    // where a minute was wanted, which is what "the seeking is way off" was.
    REQUIRE(asked >= 0);
    INFO("asked for frame " << asked << ", a minute is " << (kDsdRate * 60.0));
    CHECK(static_cast<double>(asked) == Approx(kDsdRate * 60.0).epsilon(0.001));
}
