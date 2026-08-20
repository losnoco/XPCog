// DSF and DSDIFF: the two containers DSD actually ships in.
//
// One test here is worth more than all the others, and it exists because of a
// coincidence in the fixtures. A DSF and a WavPack file holding the *same*
// recording decode to the same thing or the port is wrong -- WavPack's DSD path
// has been in this tree since M6 and is verified separately, so it stands as a
// reference implementation for the bytes a DSD decoder should produce. That
// turns "does the bit order look right" from a listening judgement into an
// equality.
//
// It is worth being clear about why bit order needs pinning at all. A one-bit
// stream read backwards is still a legal one-bit stream: nothing errors, no
// length is wrong, and the file plays. It plays as a full-scale rasp. The same
// is true of reading DSF's per-channel blocks as though they were interleaved.
// Both are the kind of mistake that a test asserting "audio came out" passes.
//
// The fixtures are SACD rips and cannot be committed, so these run against a
// directory already on the machine (`-DXPCOG_DSD_CORPUS=<path>`) and skip
// without one. The cases that need no fixture -- registration, and refusing
// things that are not DSD -- run everywhere.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <chrono>
#include <cmath>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;
namespace fs = std::filesystem;

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

#ifdef XPCOG_DSD_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_DSD_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

[[nodiscard]] std::string lowerExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

[[nodiscard]] std::vector<fs::path> findByExtension(std::string_view dotted) {
    std::vector<fs::path> found;
    if (!kHaveCorpus) {
        return found;
    }
    std::error_code                  error;
    fs::recursive_directory_iterator walk{
        corpusRoot(), fs::directory_options::skip_permission_denied, error};
    if (error) {
        return found;
    }
    for (const fs::directory_entry& entry : walk) {
        if (entry.is_regular_file(error) && lowerExtension(entry.path()) == dotted) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

/// Reads `frames` frames of raw DSD from `url`, or fewer at end of stream.
[[nodiscard]] std::vector<std::uint8_t> readDsd(const Url& url, std::size_t frames,
                                                std::int64_t   from = 0,
                                                AudioFormat*   format = nullptr,
                                                std::int64_t*  total  = nullptr) {
    std::vector<std::uint8_t>  out;
    PluginRegistry::OpenResult opened = registry().open(url);
    if (!opened) {
        return out;
    }

    const TrackProperties props = opened.decoder->properties();
    if (format != nullptr) {
        *format = props.format;
    }
    if (total != nullptr) {
        *total = props.totalFrames;
    }
    if (props.format.format != SampleFormat::DSD || props.format.channels == 0) {
        return out;
    }
    if (from != 0 && opened.decoder->seek(from) != from) {
        return out;
    }

    const std::size_t channels = props.format.channels;
    AudioChunk        chunk;
    while (out.size() < frames * channels && opened.decoder->readAudio(chunk)) {
        const std::size_t got = chunk.frameCount();
        if (got == 0) {
            break;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(chunk.bytes().data());
        out.insert(out.end(), bytes, bytes + (got * channels));
    }
    out.resize(std::min(out.size(), frames * channels));
    return out;
}

[[nodiscard]] Url writeTemp(const std::string& name, const std::string& bytes) {
    const fs::path  dir = fs::temp_directory_path() / "xpcog-dsd-tests";
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path path = dir / name;

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
    return Url::fromLocalPath(path);
}

/// Two seconds of DSD64, which is eight megabytes of one-bit audio and plenty
/// to tell a correct decode from a reversed one.
constexpr std::size_t kCompareFrames = 700000;

}  // namespace

TEST_CASE("both DSD containers are claimed", "[dsd][containers]") {
    CHECK(registry().isPlayableExtension("dsf"));
    CHECK(registry().isPlayableExtension("dff"));
    CHECK(registry().isPlayableExtension("dsdiff"));
}

TEST_CASE("something that is not DSD is declined", "[dsd][containers]") {
    CHECK_FALSE(registry().open(writeTemp("garbage.dsf", "not a DSD file at all")));
    CHECK_FALSE(registry().open(writeTemp("garbage.dff", "not a DSD file at all")));
    CHECK_FALSE(registry().open(writeTemp("empty.dsf", "")));

    // `.dsf` is claimed by codecs/sdsf as well -- Sega's Dreamcast Sound Format,
    // which is a PSF and has nothing to do with one-bit audio. Neither decoder
    // carries a priority for it, so what keeps them apart is that each declines
    // what is not its own, and the registry moves on.
    //
    // So the assertion is *not* that this fails to open. A header of "PSF" and
    // version 0x12 is what a Dreamcast rip opens with, and sdsf is entitled to
    // take it -- which it does. What matters is that this decoder did not: if
    // it had, a Dreamcast rip would be played as one-bit audio, which is noise
    // at eight times the intended length.
    PluginRegistry::OpenResult dreamcast = registry().open(writeTemp(
        "dreamcast.dsf", std::string("PSF\x12", 4) + std::string(60, '\0')));
    if (dreamcast) {
        CHECK(dreamcast.decoder->properties().format.format != SampleFormat::DSD);
    }

    // Truncated after the magic, which is where a reader that trusts its header
    // reads past the end of the file.
    CHECK_FALSE(registry().open(writeTemp("truncated.dsf", "DSD ")));
    CHECK_FALSE(registry().open(writeTemp("truncated.dff", "FRM8")));
}

TEST_CASE("a DSF reports one-bit audio at the byte rate", "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }
    const auto files = findByExtension(".dsf");
    if (files.empty()) {
        SKIP("corpus holds no .dsf");
    }

    for (const fs::path& path : files) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        const TrackProperties props = opened.decoder->properties();
        CHECK(props.format.format == SampleFormat::DSD);
        CHECK(props.format.bitsPerSample == 1);
        CHECK(props.encoding == "lossless");

        // The byte rate, not the 2.8 MHz bit rate the file states -- that is
        // this tree's convention and the reason every frame count downstream
        // stays true. A DSD64 file lands on 352,800 and DSD256 on 1,411,200.
        CHECK(props.format.sampleRate >= 352800.0);
        CHECK(props.format.sampleRate <= 2822400.0);
        CHECK(props.totalFrames > 0);
    }
}

TEST_CASE("a DSDIFF reports one-bit audio at the byte rate", "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }
    const auto files = findByExtension(".dff");
    if (files.empty()) {
        SKIP("corpus holds no .dff");
    }

    for (const fs::path& path : files) {
        INFO(path.filename().string());
        PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(path));
        REQUIRE(opened);

        const TrackProperties props = opened.decoder->properties();
        CHECK(props.format.format == SampleFormat::DSD);
        CHECK(props.format.bitsPerSample == 1);
        CHECK(props.format.sampleRate >= 352800.0);
        CHECK(props.totalFrames > 0);
    }
}

TEST_CASE("a DSF decodes to the same bits as its WavPack reference",
          "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }

    // Paired by stem: `x.dsf` beside `x.wv` is the same recording in two
    // containers, and WavPack's DSD path is already verified, so it stands as
    // the reference. Nothing else in this suite can check bit order at all.
    fs::path dsf;
    fs::path reference;
    for (const fs::path& candidate : findByExtension(".dsf")) {
        fs::path paired = candidate;
        paired.replace_extension(".wv");
        if (fs::exists(paired)) {
            dsf       = candidate;
            reference = paired;
            break;
        }
    }
    if (dsf.empty()) {
        SKIP("corpus holds no .dsf with a same-stem .wv beside it");
    }
    INFO(dsf.filename().string() << " against " << reference.filename().string());

    AudioFormat  dsfFormat{};
    AudioFormat  wvFormat{};
    std::int64_t dsfFrames = 0;
    std::int64_t wvFrames  = 0;

    const auto fromDsf = readDsd(Url::fromLocalPath(dsf), kCompareFrames, 0,
                                 &dsfFormat, &dsfFrames);
    const auto fromWv  = readDsd(Url::fromLocalPath(reference), kCompareFrames, 0,
                                 &wvFormat, &wvFrames);

    REQUIRE_FALSE(fromDsf.empty());
    REQUIRE_FALSE(fromWv.empty());

    // The containers have to agree about the stream before the bytes can mean
    // anything. A mismatch here would make the comparison below meaningless
    // rather than failing it.
    REQUIRE(dsfFormat.sampleRate == wvFormat.sampleRate);
    REQUIRE(dsfFormat.channels == wvFormat.channels);
    REQUIRE(dsfFrames == wvFrames);

    REQUIRE(fromDsf.size() == fromWv.size());
    CHECK(std::equal(fromDsf.begin(), fromDsf.end(), fromWv.begin()));
}

TEST_CASE("a DSF still matches its reference after a seek",
          "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }

    fs::path dsf;
    fs::path reference;
    for (const fs::path& candidate : findByExtension(".dsf")) {
        fs::path paired = candidate;
        paired.replace_extension(".wv");
        if (fs::exists(paired)) {
            dsf       = candidate;
            reference = paired;
            break;
        }
    }
    if (dsf.empty()) {
        SKIP("corpus holds no .dsf with a same-stem .wv beside it");
    }

    // Deliberately not on a block boundary. DSF stores 4096 frames per channel
    // per block and a seek lands inside one, so the reader has to load the
    // block and then start part-way into it -- arithmetic that a seek to zero,
    // or to a multiple of 4096, would never exercise.
    constexpr std::int64_t kTarget = (4096 * 500) + 1234;

    PluginRegistry::OpenResult opened = registry().open(Url::fromLocalPath(dsf));
    REQUIRE(opened);
    if (opened.decoder->properties().totalFrames <= kTarget + 100000) {
        SKIP("reference file is too short for the seek target");
    }

    const auto fromDsf = readDsd(Url::fromLocalPath(dsf), 100000, kTarget);
    const auto fromWv  = readDsd(Url::fromLocalPath(reference), 100000, kTarget);

    REQUIRE_FALSE(fromDsf.empty());
    REQUIRE(fromDsf.size() == fromWv.size());
    CHECK(std::equal(fromDsf.begin(), fromDsf.end(), fromWv.begin()));
}

TEST_CASE("seeking a DSD container is exact and repeatable",
          "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }

    // Both containers, because their seek arithmetic has nothing in common:
    // DSDIFF is a byte offset, DSF is a block index plus a remainder.
    for (const std::string_view extension : {".dsf", ".dff"}) {
        const auto files = findByExtension(extension);
        if (files.empty()) {
            continue;
        }
        INFO(files.front().filename().string());

        PluginRegistry::OpenResult opened =
            registry().open(Url::fromLocalPath(files.front()));
        REQUIRE(opened);
        const std::int64_t total = opened.decoder->properties().totalFrames;
        REQUIRE(total > 200000);

        constexpr std::int64_t kSomewhere = 123457;  // odd, and not a block edge
        CHECK(opened.decoder->seek(kSomewhere) == kSomewhere);

        AudioChunk first;
        REQUIRE(opened.decoder->readAudio(first));
        std::vector<std::byte> firstBytes(first.bytes().begin(), first.bytes().end());

        // Back to the start and forward again: a reader that leaves state behind
        // -- a half-consumed block, a stale file position -- gives a different
        // answer the second time.
        CHECK(opened.decoder->seek(0) == 0);
        CHECK(opened.decoder->seek(kSomewhere) == kSomewhere);

        AudioChunk again;
        REQUIRE(opened.decoder->readAudio(again));
        REQUIRE(again.frameCount() == first.frameCount());
        CHECK(std::equal(firstBytes.begin(), firstBytes.end(), again.bytes().begin()));

        // Seeking to the very end must not hang or read past it.
        CHECK(opened.decoder->seek(total) == total);
        AudioChunk atEnd;
        CHECK_FALSE(opened.decoder->readAudio(atEnd));
    }
}


TEST_CASE("both containers play through the whole chain", "[dsd][containers][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_DSD_CORPUS=<path> to run this");
    }

    // The reference comparison above proves the bytes leaving the decoder, and
    // stops there. This is the rest of the path: decimation, resampling, and the
    // engine choosing a rate a device could actually run -- the failure that
    // took two passes to find when DSD first landed, where every part was right
    // and the track was silent because 2.8 MHz is not a device rate.
    //
    // What it does *not* do is stand in for the reference comparison, and that
    // was worth finding out rather than assuming: with the bit reversal removed
    // this case still passes. The level after decimation is not a usable signal
    // for bit order, so DSDIFF -- which has no reference file here -- rests on
    // its layout being the simple one (interleaved, MSB first, no blocking) and
    // on its header having been read against a hand-parse of the container.
    // If a DSDIFF and a WavPack of one recording ever land in the same folder,
    // the comparison above picks them up with no change.
    for (const std::string_view extension : {".dsf", ".dff"}) {
        const auto files = findByExtension(extension);
        if (files.empty()) {
            continue;
        }
        INFO(files.front().filename().string());

        PluginRegistry registry;
        registerAllCodecs(registry);

        RingBuffer ring(48000 * 2);
        auto       output = makeOfflineOutput(ring);
        auto       store  = makeMemorySettingsStore();
        Settings   settings(*store);
        AudioEngine engine(registry, *output, ring, settings);

        REQUIRE(engine.play(Url::fromLocalPath(files.front())));

        // No backend opens a device at the DSD rate, so the engine has to have
        // asked for something else. miniaudio's ceiling is 384,000.
        const double deviceRate = output->negotiatedFormat().sampleRate;
        INFO("device rate " << deviceRate);
        CHECK(deviceRate > 0.0);
        CHECK(deviceRate <= 384000.0);

        for (int spin = 0; spin < 300 && capturedAudio(*output).size() < 44100 * 2;
             ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const std::vector<float> played = capturedAudio(*output);
        engine.stop();

        REQUIRE_FALSE(played.empty());

        double sum = 0.0;
        float  peak = 0.0F;
        for (const float sample : played) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
            peak = std::max(peak, std::abs(sample));
        }
        const double rms = std::sqrt(sum / static_cast<double>(played.size()));
        INFO("peak " << peak << " rms " << rms << " over " << played.size());

        // Something rather than nothing, and not the rails. Both bounds are
        // loose on purpose: the upper one is 4.0 because the filter's stated
        // gain is 2.0 and DSD is allowed to reach half modulation (see
        // test_dsd.cpp), and the lower one only has to clear digital silence --
        // this fixture is an audiophile classical recording whose first two
        // seconds peak at 0.004, so anything tighter would be asserting a
        // property of the performance.
        CHECK(peak > 0.0001F);
        CHECK(peak < 4.0F);
        CHECK(rms > 0.0);
    }
}
