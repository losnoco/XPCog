// The waveform behind the seek bar: the analyser that folds a track into
// buckets, the file those buckets are kept in, and the cache that keys the file
// to the track.
//
// The analyser is tested against a decoder built in this file rather than a
// fixture on disk, because what matters is arithmetic -- which bucket a frame
// lands in, that channels are averaged, that the RMS is over the bucket's own
// frames -- and a synthesised stream makes every one of those exact. One test
// at the end goes through the registry and a real FLAC, to prove the seam
// between the two, and skips when `flac` is not installed.

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/Waveform.hpp"
#include "xpcog/core/audio/WaveformCache.hpp"
#include "xpcog/core/audio/WaveformFile.hpp"
#include "xpcog/core/audio/WaveformProvider.hpp"

#include "../TestShell.hpp"
#include "../TestSignal.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace xpcog;

namespace {

namespace fs = std::filesystem;

constexpr double kRate = 48000.0;

/// A decoder that plays back a float buffer in chunks of a chosen size.
class BufferDecoder final : public IDecoder {
public:
    BufferDecoder(std::vector<float> interleaved, std::uint32_t channels,
                  std::int64_t declaredFrames, std::size_t chunkFrames = 4096)
        : samples_(std::move(interleaved)), channels_(channels),
          declared_(declaredFrames), chunkFrames_(chunkFrames) {}

    bool open(ISource*) override { return true; }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate = kRate;
        props.format.channels   = channels_;
        props.format.format     = SampleFormat::F32;
        props.totalFrames       = declared_;
        return props;
    }

    bool readAudio(AudioChunk& out) override {
        const std::size_t total = samples_.size() / channels_;
        if (cursor_ >= total) {
            return false;
        }
        const std::size_t frames = std::min(chunkFrames_, total - cursor_);
        out.setFormat(properties().format);
        out.assign(samples_.data() + (cursor_ * channels_), frames);
        cursor_ += frames;
        ++reads;
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void         close() override {}

    int reads = 0;

private:
    std::vector<float> samples_;
    std::uint32_t      channels_;
    std::int64_t       declared_;
    std::size_t        chunkFrames_;
    std::size_t        cursor_ = 0;
};

/// `frames` of a constant-amplitude square wave, channels alike unless `right`
/// is given. A square wave has peak == RMS, which makes both readings checkable
/// with one number.
std::vector<float> square(std::size_t frames, float left, std::optional<float> right = {}) {
    std::vector<float> out;
    out.reserve(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float sign = (i % 2 == 0) ? 1.0F : -1.0F;
        out.push_back(sign * left);
        out.push_back(sign * right.value_or(left));
    }
    return out;
}

/// A DSD128 decoder as WavPack presents one: a frame is a byte per channel and
/// the rate is the byte rate. The first `quietFrames` are 0xAA, DSD's zero,
/// and the rest 0xFF, every bit positive, which the filter turns into its
/// stated gain of 2.0. Both are exact patterns, so what a bucket should read
/// is a number rather than a judgement.
class DsdDecoder final : public IDecoder {
public:
    DsdDecoder(std::size_t quietFrames, std::size_t loudFrames, std::size_t chunkFrames = 4096)
        : quiet_(quietFrames), total_(quietFrames + loudFrames), chunkFrames_(chunkFrames) {}

    bool open(ISource*) override { return true; }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate    = 705600.0;
        props.format.channels      = 2;
        props.format.channelConfig = 0x3;
        props.format.format        = SampleFormat::DSD;
        props.format.bitsPerSample = 1;
        props.totalFrames          = static_cast<std::int64_t>(total_);
        return props;
    }

    bool readAudio(AudioChunk& out) override {
        if (cursor_ >= total_) {
            return false;
        }
        const std::size_t frames = std::min(chunkFrames_, total_ - cursor_);
        out.setFormat(properties().format);
        std::byte* bytes = out.allocFrames(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            const auto pattern = static_cast<std::byte>(cursor_ + f < quiet_ ? 0xAA : 0xFF);
            bytes[f * 2]       = pattern;
            bytes[f * 2 + 1]   = pattern;
        }
        out.setFrameCount(frames);
        cursor_ += frames;
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void         close() override {}

private:
    std::size_t quiet_;
    std::size_t total_;
    std::size_t chunkFrames_;
    std::size_t cursor_ = 0;
};

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(fs::temp_directory_path() / ("xpcog-waveform-" + name)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    fs::path write(const std::string& name, std::string_view text) const {
        const fs::path file = path_ / name;
        std::ofstream  out{file, std::ios::binary};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return file;
    }

private:
    fs::path path_;
};

WaveformSummary complete(std::uint8_t peakLevel, std::uint8_t rmsLevel, double duration = 3.0) {
    WaveformSummary s;
    s.bucketCount = kWaveformBuckets;
    s.analysed    = kWaveformBuckets;
    s.duration    = duration;
    s.peak.assign(kWaveformBuckets, peakLevel);
    s.rms.assign(kWaveformBuckets, rmsLevel);
    for (std::uint32_t i = 0; i < kWaveformBuckets; ++i) {
        s.peak[i] = static_cast<std::uint8_t>((peakLevel + i) & 0xFF);
    }
    return s;
}

}  // namespace

// --- analyser ----------------------------------------------------------------

TEST_CASE("Waveform analyser fills every bucket and reports completion", "[waveform]") {
    const std::size_t frames = kWaveformBuckets * 10;
    BufferDecoder     decoder(square(frames, 0.5F), 2, static_cast<std::int64_t>(frames));

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));

    CHECK(out.bucketCount == kWaveformBuckets);
    CHECK(out.complete());
    CHECK(out.duration == Catch::Approx(frames / kRate));
    REQUIRE(out.peak.size() == kWaveformBuckets);
    REQUIRE(out.rms.size() == kWaveformBuckets);
    for (std::uint32_t i = 0; i < kWaveformBuckets; ++i) {
        CHECK(out.peak[i] == 128);
        CHECK(out.rms[i] == 128);
    }
}

TEST_CASE("Waveform analyser puts a level change in the right bucket", "[waveform]") {
    // Loud for the first half, 6 dB quieter for the second. The boundary must
    // fall exactly between buckets 511 and 512.
    const std::size_t half = kWaveformBuckets * 8;
    std::vector<float> loud  = square(half, 1.0F);
    std::vector<float> quiet = square(half, 0.5F);
    loud.insert(loud.end(), quiet.begin(), quiet.end());

    BufferDecoder decoder(std::move(loud), 2, static_cast<std::int64_t>(2 * half), 1000);

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));

    CHECK(out.peak[0] == 255);
    CHECK(out.peak[511] == 255);
    CHECK(out.peak[512] == 128);
    CHECK(out.peak[kWaveformBuckets - 1] == 128);
    CHECK(out.rms[511] == 255);
    CHECK(out.rms[512] == 128);
}

TEST_CASE("Waveform analyser averages channels rather than summing them", "[waveform]") {
    // Left full scale, right silent: the mono mix is half.
    const std::size_t frames = kWaveformBuckets * 4;
    BufferDecoder decoder(square(frames, 1.0F, 0.0F), 2, static_cast<std::int64_t>(frames));

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));
    CHECK(out.peak[100] == 128);
    CHECK(out.rms[100] == 128);
}

TEST_CASE("Waveform analyser keeps RMS below peak for a sparse signal", "[waveform]") {
    // One full-scale sample in every eight frames, silence otherwise: peak is
    // full, RMS is sqrt(1/8).
    const std::size_t  frames = kWaveformBuckets * 8;
    std::vector<float> samples(frames * 2, 0.0F);
    for (std::size_t i = 0; i < frames; i += 8) {
        samples[i * 2]       = 1.0F;
        samples[(i * 2) + 1] = 1.0F;
    }
    BufferDecoder decoder(std::move(samples), 2, static_cast<std::int64_t>(frames));

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));
    CHECK(out.peak[3] == 255);
    CHECK(out.rms[3] == static_cast<std::uint8_t>(std::lround(std::sqrt(1.0 / 8.0) * 255.0)));
}

TEST_CASE("Waveform analyser completes a track that stops short of its declaration",
          "[waveform]") {
    // Declared twice as long as it is. The second half stays silent, but the
    // summary is still complete -- an incomplete one is never stored and never
    // triggers the prefetch.
    const std::size_t frames = kWaveformBuckets * 4;
    BufferDecoder decoder(square(frames, 0.5F), 2, static_cast<std::int64_t>(frames * 2));

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));
    CHECK(out.complete());
    CHECK(out.peak[0] == 128);
    CHECK(out.peak[kWaveformBuckets / 2] == 0);
    CHECK(out.peak[kWaveformBuckets - 1] == 0);
}

TEST_CASE("Waveform analyser folds a track that overruns its declaration into the tail",
          "[waveform]") {
    const std::size_t frames = kWaveformBuckets * 4;
    BufferDecoder decoder(square(frames, 0.5F), 2, static_cast<std::int64_t>(frames / 2));

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));
    CHECK(out.complete());
    CHECK(out.peak[kWaveformBuckets - 1] == 128);
}

TEST_CASE("Waveform analyser decimates DSD rather than refusing it", "[waveform][dsd]") {
    // Half silence, half full modulation: the boundary falls between buckets
    // 511 and 512, and each half is long enough that the filter's 64 taps of
    // settling are a rounding error in the bucket they land in.
    const std::size_t half = kWaveformBuckets * 64;
    DsdDecoder        decoder(half, half);

    WaveformSummary out;
    REQUIRE(analyseWaveform(decoder, out, [] { return false; }));
    CHECK(out.complete());
    // Frames are bytes, so the declared length is in bytes at the byte rate.
    CHECK(out.duration == Catch::Approx(static_cast<double>(2 * half) / 705600.0));

    // DSD's zero comes through as nothing, not as the step to negative full
    // scale that priming the filter with zero bytes would produce.
    CHECK(out.peak[0] == 0);
    CHECK(out.peak[510] == 0);
    CHECK(out.rms[510] == 0);
    // Full modulation at the filter's gain of 2.0 pins the top of the scale.
    CHECK(out.peak[600] == 255);
    CHECK(out.rms[600] == 255);
    CHECK(out.rms[kWaveformBuckets - 1] == 255);
}

TEST_CASE("Waveform analyser stops when cancelled", "[waveform]") {
    const std::size_t frames = kWaveformBuckets * 16;
    BufferDecoder decoder(square(frames, 0.5F), 2, static_cast<std::int64_t>(frames), 256);

    int             polls = 0;
    WaveformSummary out;
    CHECK_FALSE(analyseWaveform(decoder, out, [&] { return ++polls > 3; }));
    CHECK(decoder.reads == 3);
    CHECK_FALSE(out.complete());
}

TEST_CASE("Waveform analyser refuses a track with no declared length", "[waveform]") {
    BufferDecoder decoder(square(1000, 0.5F), 2, 0);

    WaveformSummary out;
    CHECK_FALSE(analyseWaveform(decoder, out, [] { return false; }));
}

TEST_CASE("Waveform analyser reports progress with only finished buckets", "[waveform]") {
    // Progress is throttled by wall clock, so this only asserts what it says
    // when it does speak: never more buckets than frames have covered, and never
    // a complete summary before the end.
    const std::size_t frames = kWaveformBuckets * 64;
    BufferDecoder decoder(square(frames, 0.5F), 2, static_cast<std::int64_t>(frames), 64);

    std::uint32_t   last   = 0;
    bool            ordered = true;
    WaveformSummary out;
    REQUIRE(analyseWaveform(
        decoder, out, [] { return false; },
        [&](const WaveformSummary& partial) {
            if (partial.analysed < last || partial.complete()) {
                ordered = false;
            }
            last = partial.analysed;
            for (std::uint32_t i = 0; i < partial.analysed; ++i) {
                if (partial.peak[i] != 128) {
                    ordered = false;
                }
            }
        }));
    CHECK(ordered);
    CHECK(out.complete());
}

// --- file format ------------------------------------------------------------

TEST_CASE("Waveform file round-trips a summary and its stamp", "[waveform]") {
    const WaveformSummary    in = complete(10, 200, 271.5);
    const PluginCache::Stamp stamp{123456789, 987654321};

    const std::vector<std::byte> bytes = encodeWaveform(in, stamp);
    CHECK(bytes.size() == waveformFileSize(kWaveformBuckets));
    CHECK(bytes.size() == 2084);

    const auto record = decodeWaveform(bytes);
    REQUIRE(record.has_value());
    CHECK(record->stamp == stamp);
    CHECK(record->summary.bucketCount == in.bucketCount);
    CHECK(record->summary.complete());
    CHECK(record->summary.duration == in.duration);
    CHECK(record->summary.peak == in.peak);
    CHECK(record->summary.rms == in.rms);
}

TEST_CASE("Waveform file rejects what is not one", "[waveform]") {
    std::vector<std::byte> bytes = encodeWaveform(complete(1, 2), {1, 2});

    SECTION("wrong magic") {
        bytes[0] = std::byte{'x'};
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("unknown version") {
        bytes[4] = std::byte{2};
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("unknown layout") {
        bytes[5] = std::byte{7};
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("truncated") {
        bytes.pop_back();
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("too long for its bucket count") {
        bytes.push_back(std::byte{0});
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("shorter than a header") {
        bytes.resize(20);
        CHECK_FALSE(decodeWaveform(bytes).has_value());
    }
    SECTION("empty") {
        CHECK_FALSE(decodeWaveform({}).has_value());
    }
}

// --- cache -------------------------------------------------------------------

TEST_CASE("Waveform cache stores and loads by URL and stamp", "[waveform]") {
    TempDir dir("cache");
    const fs::path file = dir.write("song.flac", "not really audio, but it has a stamp");
    const Url      url  = Url::fromLocalPath(file);

    const WaveformCache cache(dir.path() / "waveforms");
    CHECK_FALSE(cache.load(url).has_value());

    const WaveformSummary summary = complete(30, 20);
    REQUIRE(cache.store(url, summary));

    const auto back = cache.load(url);
    REQUIRE(back.has_value());
    CHECK(back->peak == summary.peak);
    CHECK(back->rms == summary.rms);
    CHECK(back->complete());

    const std::string key = WaveformCache::keyFor(url, PluginCache::stampFor(url));
    CHECK(key.size() == 32 + 5);
    CHECK(fs::exists(dir.path() / "waveforms" / key));
    CHECK(fs::file_size(dir.path() / "waveforms" / key) == 2084);
    CHECK_FALSE(fs::exists(dir.path() / "waveforms" / (key + ".tmp")));
}

TEST_CASE("Waveform cache misses once the file changes", "[waveform]") {
    TempDir dir("stale");
    const fs::path file = dir.write("song.flac", "first");
    const Url      url  = Url::fromLocalPath(file);

    const WaveformCache cache(dir.path() / "waveforms");
    REQUIRE(cache.store(url, complete(1, 1)));
    REQUIRE(cache.load(url).has_value());

    // A different size is a different stamp, whatever the clock says.
    dir.write("song.flac", "second, and longer");
    CHECK_FALSE(cache.load(url).has_value());
}

TEST_CASE("Waveform cache keeps cue tracks apart", "[waveform]") {
    TempDir dir("fragments");
    const fs::path sheet = dir.write("album.cue", "FILE \"album.flac\" WAVE");
    const Url      one   = Url::fromLocalPath(sheet).withFragment("1");
    const Url      two   = Url::fromLocalPath(sheet).withFragment("2");

    const WaveformCache cache(dir.path() / "waveforms");
    REQUIRE(cache.store(one, complete(10, 10)));
    CHECK_FALSE(cache.load(two).has_value());
    REQUIRE(cache.store(two, complete(20, 20)));

    const auto a = cache.load(one);
    const auto b = cache.load(two);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->rms[0] == 10);
    CHECK(b->rms[0] == 20);
}

TEST_CASE("Waveform cache never writes for a URL it cannot stamp", "[waveform]") {
    TempDir dir("remote");
    const WaveformCache cache(dir.path() / "waveforms");

    const auto remote = Url::parse("http://example.invalid/stream.mp3");
    REQUIRE(remote.has_value());
    CHECK_FALSE(cache.store(*remote, complete(1, 1)));
    CHECK_FALSE(cache.load(*remote).has_value());

    const Url missing = Url::fromLocalPath(dir.path() / "does-not-exist.flac");
    CHECK_FALSE(cache.store(missing, complete(1, 1)));
    CHECK_FALSE(fs::exists(dir.path() / "waveforms"));
}

TEST_CASE("Waveform cache refuses an incomplete summary", "[waveform]") {
    TempDir dir("partial");
    const fs::path file = dir.write("song.flac", "bytes");
    const Url      url  = Url::fromLocalPath(file);

    WaveformSummary partial = complete(1, 1);
    partial.analysed        = kWaveformBuckets / 2;

    const WaveformCache cache(dir.path() / "waveforms");
    CHECK_FALSE(cache.store(url, partial));
    CHECK_FALSE(cache.load(url).has_value());
}

TEST_CASE("Waveform cache ignores a file whose header names another stamp", "[waveform]") {
    TempDir dir("mismatch");
    const fs::path file = dir.write("song.flac", "bytes");
    const Url      url  = Url::fromLocalPath(file);
    const auto     stamp = PluginCache::stampFor(url);

    const fs::path waveforms = dir.path() / "waveforms";
    fs::create_directories(waveforms);
    const std::vector<std::byte> bytes = encodeWaveform(complete(1, 1), {stamp.modifiedSeconds + 1, stamp.sizeBytes});
    std::ofstream out{waveforms / WaveformCache::keyFor(url, stamp), std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();

    const WaveformCache cache(waveforms);
    CHECK_FALSE(cache.load(url).has_value());
}

// --- through the registry ---------------------------------------------------

namespace {

fs::path fixtureDir() {
    static const fs::path dir = [] {
        auto path = fs::temp_directory_path() / "xpcog-waveform-fixtures";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

/// A 16-bit stereo WAV: `loudFrames` of a sine at full amplitude followed by the
/// same again at half, so the second half of the bar is 6 dB down.
fs::path writeWav(const std::string& name, int loudFrames) {
    const auto   path = fixtureDir() / name;
    const double rate = 44100.0;

    std::vector<std::int16_t> samples;
    samples.reserve(static_cast<std::size_t>(loudFrames) * 4);
    for (int i = 0; i < 2 * loudFrames; ++i) {
        const double amplitude = i < loudFrames ? 32000.0 : 16000.0;
        const double t         = static_cast<double>(i) / rate;
        const auto   v = static_cast<std::int16_t>(amplitude * std::sin(xpcog::test::kTwoPi * 440.0 * t));
        samples.push_back(v);
        samples.push_back(v);
    }

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 1, dataBytes, f);
    std::fclose(f);
    return path;
}

std::optional<fs::path> makeFlac(const std::string& name, int loudFrames) {
    if (!xpcog::test::haveTool("flac")) {
        return std::nullopt;
    }
    const auto wav  = writeWav(name + ".wav", loudFrames);
    const auto flac = fixtureDir() / (name + ".flac");
    const std::string command = "flac -s -f --totally-silent -o \"" + flac.string() + "\" \"" +
                                wav.string() + "\"" + xpcog::test::kSilenceStderr;
    if (std::system(command.c_str()) != 0 || !fs::exists(flac)) {
        return std::nullopt;
    }
    return flac;
}

PluginRegistry& registry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

}  // namespace

TEST_CASE("Waveform analyser reads a real file through the registry", "[waveform]") {
    const auto flac = makeFlac("halves", 44100 * 2);
    if (!flac) {
        SKIP("the `flac` command-line tool is not available");
    }

    auto opened = registry().open(Url::fromLocalPath(*flac), SkipCue::No, LoopPolicy::Never);
    REQUIRE(opened);

    WaveformSummary out;
    REQUIRE(analyseWaveform(*opened.decoder, out, [] { return false; }));
    CHECK(out.complete());
    CHECK(out.duration == Catch::Approx(4.0).margin(0.01));

    // Peak of a sine is its amplitude; RMS is amplitude / sqrt(2). Read away
    // from the boundary and the ends, where a bucket can straddle the change.
    // The RMS is averaged over a run of buckets: a bucket holds 1.7 cycles, so
    // any one bucket's RMS depends on the phase it starts at by several
    // percent, and the run is what averages that out.
    const double loudPeak  = 32000.0 / 32768.0;
    const double quietPeak = 16000.0 / 32768.0;
    CHECK(out.peak[100] == Catch::Approx(loudPeak * 255).margin(2));
    CHECK(out.peak[900] == Catch::Approx(quietPeak * 255).margin(2));

    const auto meanRms = [&](std::uint32_t from, std::uint32_t to) {
        double sum = 0.0;
        for (std::uint32_t i = from; i < to; ++i) {
            sum += out.rms[i];
        }
        return sum / (to - from);
    };
    CHECK(meanRms(50, 450) == Catch::Approx(loudPeak / std::sqrt(2.0) * 255).margin(2));
    CHECK(meanRms(562, 962) == Catch::Approx(quietPeak / std::sqrt(2.0) * 255).margin(2));
}

TEST_CASE("Waveform analyser reads a real DSD file through the registry",
          "[waveform][dsd]") {
#ifdef XPCOG_DSD_FILE
    const fs::path file{XPCOG_DSD_FILE};
#else
    const fs::path file;
#endif
    if (file.empty() || !fs::exists(file)) {
        SKIP("no DSD file: configure with -DXPCOG_DSD_FILE=<path to a DSD .wv>");
    }

    // The whole file, which for an SACD rip is minutes of DSD; what this proves
    // is that a real decoder's frames and the filter's output agree on what a
    // frame is, which the synthetic decoder cannot. Progress is asked for so
    // the partial snapshots run too.
    auto opened = registry().open(Url::fromLocalPath(file), SkipCue::No, LoopPolicy::Never);
    REQUIRE(opened);
    const TrackProperties props = opened.decoder->properties();
    REQUIRE(props.format.format == SampleFormat::DSD);

    WaveformSummary out;
    int             snapshots = 0;
    REQUIRE(analyseWaveform(
        *opened.decoder, out, [] { return false; },
        [&](const WaveformSummary& partial) {
            ++snapshots;
            CHECK(partial.analysed <= kWaveformBuckets);
        }));
    CHECK(out.complete());
    CHECK(out.duration == Catch::Approx(props.duration()));
    CHECK(snapshots > 0);

    // Music: something in most buckets, and nowhere the full-scale wall that
    // reading the bytes as PCM would give.
    std::uint32_t lit = 0;
    std::uint32_t pinned = 0;
    for (std::uint32_t i = 0; i < kWaveformBuckets; ++i) {
        CHECK(out.rms[i] <= out.peak[i]);
        lit += out.peak[i] > 0 ? 1 : 0;
        pinned += out.rms[i] == 255 ? 1 : 0;
    }
    CHECK(lit > kWaveformBuckets / 2);
    CHECK(pinned < kWaveformBuckets / 10);
}

// --- provider ----------------------------------------------------------------
//
// The seam between the worker and the interface. What is asserted is the same
// as for ScanTask: nothing reaches a slot except through the dispatcher, a
// superseded job's snapshots are dropped even when they were already queued,
// and a provider destroyed mid-track takes its thread with it.

namespace {

/// Emits `frames` of a square wave, one chunk at a time, with an optional
/// pause per chunk so a job can be caught mid-track.
class ToneDecoder final : public IDecoder {
public:
    static inline std::atomic<int>          chunkDelayMs{0};
    static inline std::atomic<std::int64_t> frames{kWaveformBuckets * 4};
    static inline std::atomic<int>          interrupts{0};

    bool open(ISource*) override { return true; }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format.sampleRate = kRate;
        props.format.channels   = 1;
        props.format.format     = SampleFormat::F32;
        props.totalFrames       = frames.load();
        return props;
    }

    bool readAudio(AudioChunk& out) override {
        const std::int64_t total = frames.load();
        if (cursor_ >= total) {
            return false;
        }
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(4096, total - cursor_));
        std::vector<float> samples(count);
        for (std::size_t i = 0; i < count; ++i) {
            samples[i] = (i % 2 == 0) ? 0.5F : -0.5F;
        }
        out.setFormat(properties().format);
        out.assign(samples.data(), count);
        cursor_ += static_cast<std::int64_t>(count);
        if (const int delay = chunkDelayMs.load(); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }
        return true;
    }

    std::int64_t seek(std::int64_t) override { return -1; }
    void         close() override {}
    void         interrupt() override { ++interrupts; }

private:
    std::int64_t cursor_ = 0;
};

class AnySource final : public ISource {
public:
    bool open(const Url& url) override {
        url_ = url;
        return true;
    }
    [[nodiscard]] bool seekable() const override { return true; }
    bool seek(std::int64_t, int) override { return true; }
    [[nodiscard]] std::int64_t tell() const override { return 0; }
    std::int64_t read(void*, std::int64_t) override { return 0; }
    void close() override {}
    [[nodiscard]] const Url& url() const override { return url_; }

private:
    Url url_;
};

constexpr std::string_view kFileScheme[]   = {"file"};
constexpr std::string_view kToneExtension[] = {"tone"};

PluginRegistry& toneRegistry() {
    static PluginRegistry instance;
    static const bool     once = [] {
        instance.addSource({
            .name    = "AnySource",
            .schemes = kFileScheme,
            .create  = []() -> SourcePtr { return std::make_unique<AnySource>(); },
        });
        instance.addDecoder({
            .name       = "ToneDecoder",
            .extensions = kToneExtension,
            .mimeTypes  = {},
            .create     = []() -> DecoderPtr { return std::make_unique<ToneDecoder>(); },
        });
        instance.freeze();
        return true;
    }();
    (void)once;
    return instance;
}

/// A dispatcher that is a queue, drained where the test chooses.
class Queue {
public:
    Dispatcher dispatcher() {
        return [this](std::function<void()> action) {
            const std::lock_guard lock(mutex_);
            queued_.push_back(std::move(action));
        };
    }

    /// Runs what has been queued so far. Returns how many.
    std::size_t drain() {
        std::vector<std::function<void()>> batch;
        {
            const std::lock_guard lock(mutex_);
            batch.swap(queued_);
        }
        for (auto& action : batch) {
            action();
        }
        return batch.size();
    }

    /// Drains until `done()` or `limit` passes.
    template <typename Done>
    bool drainUntil(Done done, std::chrono::milliseconds limit = std::chrono::seconds{10}) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        drain();
        return done();
    }

    [[nodiscard]] std::size_t pending() {
        const std::lock_guard lock(mutex_);
        return queued_.size();
    }

private:
    std::mutex                         mutex_;
    std::vector<std::function<void()>> queued_;
};

struct Update {
    Url                                    url;
    std::shared_ptr<const WaveformSummary> summary;
};

struct ProviderFixture {
    TempDir dir{"provider"};
    Queue   queue;
    std::vector<Update> updates;
    std::unique_ptr<WaveformProvider> provider;
    Subscription subscription;

    ProviderFixture() {
        ToneDecoder::chunkDelayMs = 0;
        ToneDecoder::frames       = kWaveformBuckets * 4;
        ToneDecoder::interrupts   = 0;
        provider = std::make_unique<WaveformProvider>(
            toneRegistry(), WaveformCache{dir.path() / "waveforms"}, queue.dispatcher());
        subscription = provider->updated().connect(
            [this](const Url& url, const std::shared_ptr<const WaveformSummary>& summary) {
                updates.push_back({url, summary});
            });
    }

    Url track(const std::string& name) {
        return Url::fromLocalPath(dir.write(name, "a stamp to key on: " + name));
    }

    [[nodiscard]] bool completeFor(const Url& url) const {
        return std::any_of(updates.begin(), updates.end(), [&](const Update& u) {
            return u.url.toString() == url.toString() && u.summary->complete();
        });
    }

    [[nodiscard]] std::size_t countFor(const Url& url) const {
        return static_cast<std::size_t>(std::count_if(updates.begin(), updates.end(), [&](const Update& u) {
            return u.url.toString() == url.toString();
        }));
    }
};

}  // namespace

TEST_CASE("Waveform provider analyses a track and hands the result to the dispatcher",
          "[waveform]") {
    ProviderFixture f;
    const Url       url = f.track("one.tone");

    f.provider->request(url);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(url); }));

    REQUIRE_FALSE(f.updates.empty());
    CHECK(f.updates.back().url.toString() == url.toString());
    CHECK(f.updates.back().summary->complete());
    CHECK(f.updates.back().summary->peak[10] == 128);

    // Kept, and answered from disk the second time: one update, complete.
    f.updates.clear();
    f.provider->request(url);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(url); }));
    CHECK(f.countFor(url) == 1);
}

TEST_CASE("Waveform provider publishes nothing off the dispatcher", "[waveform]") {
    ProviderFixture f;
    const Url       url = f.track("quiet.tone");

    f.provider->request(url);
    // Give the worker time to finish without draining.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (f.queue.pending() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(f.updates.empty());
    CHECK(f.queue.pending() > 0);
    f.queue.drain();
    CHECK(f.completeFor(url));
}

TEST_CASE("Waveform provider drops a superseded job's snapshots, even queued ones",
          "[waveform]") {
    ProviderFixture f;
    ToneDecoder::chunkDelayMs = 5;
    ToneDecoder::frames       = 4096 * 200;  // a second of chunks
    const Url slow = f.track("slow.tone");
    const Url next = f.track("next.tone");

    f.provider->request(slow);
    // Let the first job get going and queue some snapshots, but do not drain.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (f.queue.pending() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(f.queue.pending() > 0);

    ToneDecoder::chunkDelayMs = 0;
    f.provider->request(next);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(next); }));

    CHECK(f.countFor(slow) == 0);
    CHECK(ToneDecoder::interrupts.load() >= 1);
}

TEST_CASE("Waveform provider runs the prefetch after the request, not beside it",
          "[waveform]") {
    ProviderFixture f;
    const Url       now   = f.track("now.tone");
    const Url       after = f.track("after.tone");

    f.provider->request(now);
    f.provider->prefetch(after);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(after); }));

    CHECK(f.completeFor(now));
    const auto firstAfter = std::find_if(f.updates.begin(), f.updates.end(), [&](const Update& u) {
        return u.url.toString() == after.toString();
    });
    const auto lastNow = std::find_if(f.updates.rbegin(), f.updates.rend(), [&](const Update& u) {
        return u.url.toString() == now.toString();
    });
    REQUIRE(firstAfter != f.updates.end());
    REQUIRE(lastNow != f.updates.rend());
    CHECK(std::distance(f.updates.begin(), firstAfter) >
          std::distance(f.updates.begin(), lastNow.base()) - 1);

    // A prefetch asked for once the request is done starts at once.
    f.updates.clear();
    const Url later = f.track("later.tone");
    f.provider->prefetch(later);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(later); }));
}

TEST_CASE("Waveform provider forgets a prefetch when a new request arrives", "[waveform]") {
    ProviderFixture f;
    ToneDecoder::chunkDelayMs = 5;
    ToneDecoder::frames       = 4096 * 40;
    const Url first  = f.track("first.tone");
    const Url guess  = f.track("guess.tone");
    const Url second = f.track("second.tone");

    f.provider->request(first);
    f.provider->prefetch(guess);
    ToneDecoder::chunkDelayMs = 0;
    f.provider->request(second);
    REQUIRE(f.queue.drainUntil([&] { return f.completeFor(second); }));

    CHECK(f.countFor(guess) == 0);
    CHECK(f.countFor(first) == 0);
}

TEST_CASE("Waveform provider says nothing after cancel", "[waveform]") {
    ProviderFixture f;
    ToneDecoder::chunkDelayMs = 5;
    ToneDecoder::frames       = 4096 * 40;
    const Url url = f.track("cancelled.tone");

    f.provider->request(url);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    f.provider->cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    f.queue.drain();
    CHECK(f.updates.empty());
}

TEST_CASE("Waveform provider destroyed mid-track leaves nothing behind", "[waveform]") {
    ProviderFixture f;
    ToneDecoder::chunkDelayMs = 5;
    ToneDecoder::frames       = 4096 * 200;
    const Url url = f.track("orphan.tone");

    f.provider->request(url);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto before = std::chrono::steady_clock::now();
    f.provider.reset();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds{2});

    // Whatever was queued before the destructor ran finds no owner.
    f.queue.drain();
    CHECK(f.updates.empty());
}
