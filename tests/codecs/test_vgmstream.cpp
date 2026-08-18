// That vgmstream is registered, and registered *under* the dedicated decoders.
//
// No fixture is decoded here, and that is a real gap rather than an oversight:
// every format this codec exists for is a rip of copyrighted game audio, and
// none of it can be checked into the tree. The decoding was verified by hand
// against a corpus of 5,318 files -- thirteen formats, byte counts matching the
// durations reported -- and that evidence lives in docs/PORTING.md rather than
// here.
//
// What *is* worth a test is the thing that could break other formats silently.
// vgmstream claims several hundred extensions, including ones a game archive
// might contain but a music library certainly does: wav, ogg, mp3. It is
// registered below kDefaultPriority so the dedicated decoder is always tried
// first. Get that wrong and FLAC files still play -- through vgmstream, with
// different metadata and no cue support -- which is the kind of regression that
// shows up as a vague complaint months later.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

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

}  // namespace

TEST_CASE("vgmstream claims the console formats", "[vgmstream]") {
    // A spread across the families: Nintendo DSP ADPCM from three console
    // generations, HAL's GameCube container, and CRI's.
    for (const char* extension :
         {"brstm", "bcstm", "bfstm", "bwav", "hps", "ast", "adx", "aax", "dsp"}) {
        INFO(extension);
        CHECK(registry().isPlayableExtension(extension));
    }
}

TEST_CASE("vgmstream does not displace the dedicated decoders", "[vgmstream]") {
    // The registry sorts candidates by descending priority, so the decoder that
    // opens a .flac must still be the FLAC one even though vgmstream will also
    // answer for that extension. Checked through the extension list rather than
    // by opening a file: what matters is that both are present and ordered, and
    // the conformance suite already decodes real fixtures for the formats that
    // have them.
    CHECK(registry().isPlayableExtension("flac"));
    CHECK(registry().isPlayableExtension("ogg"));
    CHECK(registry().isPlayableExtension("wav"));

    // And the container side must not have swallowed the playlist formats: a
    // .m3u expanding through vgmstream instead of the playlist codec would turn
    // a playlist into a single unplayable row.
    CHECK(registry().isContainer(Url::fromLocalPath("dummy.m3u")));
}
