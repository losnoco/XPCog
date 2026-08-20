// `silence://<seconds>` -- a track that is a measured amount of nothing.
//
// Everything here runs everywhere: the format has no bytes, so there is no
// fixture and no corpus, and the whole of it is a URL parser and a frame count.
//
// The case that earns its place is that the track **ends**. Cog clamps the
// frame count to what is left and then hands back a zero-frame chunk for ever,
// which its engine happens to treat as an ending; here the read says so. A
// decoder that never finishes is the failure this cannot be allowed to have,
// because the reason a silence track exists at all is that something else has
// already gone wrong and the chain has to keep moving.

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>  // SEEK_SET
#include <cstring>
#include <string>
#include <vector>

using namespace xpcog;

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

[[nodiscard]] Url url(const std::string& text) {
    const auto parsed = Url::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// Frames the decoder reports for a URL, or -1 if it will not open.
[[nodiscard]] std::int64_t framesFor(const std::string& text) {
    PluginRegistry::OpenResult opened = registry().open(url(text));
    if (!opened) {
        return -1;
    }
    return opened.decoder->properties().totalFrames;
}

constexpr std::int64_t kRate     = 44100;
constexpr std::size_t  kChannels = 2;

}  // namespace

TEST_CASE("a silence URL opens through its own source", "[silence]") {
    PluginRegistry::OpenResult opened = registry().open(url("silence://10"));
    REQUIRE(opened);

    // The decoder is found by MIME type rather than by extension -- there is no
    // extension on `silence://10`, and this is the only place in the registry
    // where the MIME lookup is the primary path rather than a fallback.
    REQUIRE(opened.source);
    CHECK(opened.source->mimeType() == "audio/x-silence");

    const TrackProperties props = opened.decoder->properties();
    CHECK(props.codec == "Silence");
    CHECK(props.format.sampleRate == 44100.0);
    CHECK(props.format.channels == 2);
    CHECK(props.format.format == SampleFormat::F32);
    CHECK(props.seekable);
    CHECK(props.totalFrames == 10 * kRate);
}

TEST_CASE("the duration comes off the URL", "[silence]") {
    CHECK(framesFor("silence://10") == 10 * kRate);
    CHECK(framesFor("silence://1") == kRate);

    // Cog parses with -intValue, so this is two seconds there. Nothing Cog
    // writes has a fraction in it -- both of its call sites hardcode
    // `silence://10` -- so no existing URL changes meaning.
    CHECK(framesFor("silence://2.5") == static_cast<std::int64_t>(2.5 * 44100));

    // Without the authority slashes. `silence:30` parses to the same URL and
    // there is no reason to refuse it.
    CHECK(framesFor("silence:30") == 30 * kRate);

    // A fragment is ignored. Containers here append them freely and one may
    // reach this by accident.
    CHECK(framesFor("silence://5#0") == 5 * kRate);
}

TEST_CASE("a URL that says nothing useful still plays", "[silence]") {
    // Ten seconds is Cog's default and is what both of its call sites ask for
    // by name. Falling back rather than refusing matters more here than in any
    // other decoder: this URL exists *because* something else failed, so
    // failing again is the one answer that helps nobody.
    CHECK(framesFor("silence://") == 10 * kRate);
    CHECK(framesFor("silence://abc") == 10 * kRate);
    CHECK(framesFor("silence://-5") == 10 * kRate);
    CHECK(framesFor("silence://0") == 10 * kRate);
}

TEST_CASE("an absurd duration is clamped rather than overflowing", "[silence]") {
    // The number comes off a URL that may have been typed. An hour is not a
    // limit anybody should meet; a frame count that wraps is a limit everybody
    // downstream would.
    CHECK(framesFor("silence://99999999999") == 3600 * kRate);
    CHECK(framesFor("silence://3600") == 3600 * kRate);
    CHECK(framesFor("silence://3601") == 3600 * kRate);
}

TEST_CASE("the track is silent and then it ends", "[silence]") {
    PluginRegistry::OpenResult opened = registry().open(url("silence://1"));
    REQUIRE(opened);

    std::int64_t frames = 0;
    bool         quiet  = true;
    AudioChunk   chunk;
    // Bounded so a decoder that never finishes fails this rather than hanging
    // the suite -- which is exactly the fault being tested for.
    int reads = 0;
    while (opened.decoder->readAudio(chunk) && ++reads < 10000) {
        const std::size_t got = chunk.frameCount();
        REQUIRE(got > 0);  // A zero-frame chunk is not an ending, it is a stall.
        frames += static_cast<std::int64_t>(got);

        const auto* samples = reinterpret_cast<const float*>(chunk.bytes().data());
        for (std::size_t i = 0; i < got * kChannels; ++i) {
            quiet = quiet && samples[i] == 0.0F;
        }
    }

    CHECK(quiet);
    CHECK(frames == kRate);
    // And it stays ended.
    CHECK_FALSE(opened.decoder->readAudio(chunk));
}

TEST_CASE("seeking moves the remaining length", "[silence]") {
    PluginRegistry::OpenResult opened = registry().open(url("silence://10"));
    REQUIRE(opened);

    CHECK(opened.decoder->seek(5 * kRate) == 5 * kRate);

    // Past the end clamps to the end rather than erroring, and what is left is
    // nothing.
    CHECK(opened.decoder->seek(99 * kRate) == 10 * kRate);
    AudioChunk chunk;
    CHECK_FALSE(opened.decoder->readAudio(chunk));

    // And back to the start plays the whole thing again.
    CHECK(opened.decoder->seek(0) == 0);
    CHECK(opened.decoder->readAudio(chunk));
    CHECK(chunk.frameCount() > 0);
}

TEST_CASE("the source reads zeros forever", "[silence]") {
    // Nothing reads it -- the decoder synthesises from the URL -- but the
    // registry opens a source before it picks a decoder, and anything that
    // probes a few bytes on the way past has to get bytes rather than an error.
    SourcePtr source = registry().makeSource(url("silence://10"));
    REQUIRE(source);
    REQUIRE(source->open(url("silence://10")));

    std::vector<unsigned char> buffer(64, 0xAB);
    CHECK(source->read(buffer.data(), static_cast<std::int64_t>(buffer.size())) ==
          static_cast<std::int64_t>(buffer.size()));
    CHECK(std::all_of(buffer.begin(), buffer.end(),
                      [](unsigned char c) { return c == 0; }));

    CHECK(source->seekable());
    CHECK(source->seek(1234, SEEK_SET));
}
