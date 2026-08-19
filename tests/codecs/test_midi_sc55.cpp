// Stage 3 of the MIDI port: a Roland SC-55mkII, booted from its own firmware.
//
// These need the ROMs, which are 3.6 MB of commercial Roland firmware and are
// not something this repository can carry. Point `XPCOG_SC55_ROMS` at either
// the folder holding them or the archive they arrived in and the cases run;
// without it they skip, which is the same shape the PSF, SID and vgmstream
// corpora use.
//
// What is worth testing here is not "does it make a sound" -- it is a machine
// running real firmware, and if the ROMs load at all it will. It is the three
// places this synthesiser refuses to behave like the others:
//
//   The ROM set is identified by content. A dump is named after Roland part
//   numbers, and nuked-sc55 asks for `rom1.bin`. Nothing but the hash connects
//   the two, and a table of ten hashes is exactly the kind of thing that gets
//   one character wrong.
//
//   The sample rate is the hardware's. Every other synthesised format in this
//   player renders at `synthSampleRate`; this one cannot, and a decoder that
//   quietly used 44100 anyway would resample the output of a machine that never
//   ran at that rate.
//
//   A missing ROM set falls back rather than refusing. The file still plays, on
//   the OPL, and the track properties say so -- which is the only way anyone
//   could tell.

#include "MidiFixtures.hpp"

#include "midi/Sc55Roms.hpp"
#include "midi/Sc55Synth.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/PanelFeed.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::testing;

namespace {

#ifdef XPCOG_SC55_ROMS
constexpr bool kHaveRoms = true;
[[nodiscard]] fs::path romPath() { return fs::path{XPCOG_SC55_ROMS}; }
#else
constexpr bool kHaveRoms = false;
[[nodiscard]] fs::path romPath() { return {}; }
#endif

struct Harness {
    std::unique_ptr<ISettingsStore> store = makeMemorySettingsStore();
    Settings                        settings{*store};
    PluginRegistry                  registry;

    Harness() {
        registerAllCodecs(registry);
        registry.setSettings(&settings);
    }
};

fs::path writeFixture(const std::string& name,
                      const std::vector<std::uint8_t>& bytes) {
    const fs::path dir = fs::temp_directory_path() / "xpcog-midi-tests";
    fs::create_directories(dir);
    const fs::path path = dir / name;
    std::FILE*     file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return path;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
};

[[nodiscard]] Decoded decode(const PluginRegistry& registry, const Url& url,
                             std::size_t limitSamples) {
    Decoded out;
    PluginRegistry::OpenResult opened = registry.open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();

    AudioChunk chunk;
    while (out.samples.size() < limitSamples && opened.decoder->readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t channels = chunk.format().channels;
        const std::size_t at       = out.samples.size();
        out.samples.resize(at + frames * channels);
        std::memcpy(out.samples.data() + at, chunk.bytes().data(),
                    frames * channels * sizeof(std::int16_t));
    }
    return out;
}

[[nodiscard]] int peak(const std::vector<std::int16_t>& samples) {
    int highest = 0;
    for (const std::int16_t sample : samples) {
        highest = std::max(highest, std::abs(static_cast<int>(sample)));
    }
    return highest;
}

constexpr int kAudible = 500;

}  // namespace

TEST_CASE("a ROM set is identified by hash, whatever it is called", "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }
    INFO("reading " << (fs::is_directory(romPath()) ? "a folder" : "an archive"));

    const auto roms = codecs::loadSc55Roms(romPath());
    REQUIRE(roms.has_value());

    // The names in the set are the ones nuked-sc55 will ask for, which are
    // never the names on disk: a dump of an SC-55mkII is five files called
    // things like r15199858_main_mcu.bin.
    CHECK(roms->device == "SC-55mk2");
    CHECK(roms->roms.size() == 5);
    for (const char* wanted :
         {"rom1.bin", "rom2.bin", "rom_sm.bin", "waverom1.bin", "waverom2.bin"}) {
        INFO(wanted);
        CHECK(roms->find(wanted) != nullptr);
    }
}

TEST_CASE("nothing that is not a ROM set is mistaken for one", "[midi][sc55]") {
    // A MIDI file is not a ROM, and neither is a folder of them. This is the
    // case that stops a mistyped setting booting a machine out of whatever
    // happened to be lying there.
    const auto file = writeFixture("not-a-rom.mid", tinyMidi());
    CHECK_FALSE(codecs::loadSc55Roms(file).has_value());
    CHECK_FALSE(codecs::loadSc55Roms(file.parent_path()).has_value());
    CHECK_FALSE(codecs::loadSc55Roms({}).has_value());
}

TEST_CASE("the SC-55 renders at its own rate, not the setting's", "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    Harness harness;
    harness.settings.setMidiPlugin("NukeSc55");
    harness.settings.setMidiRomPath(romPath().string());
    // Deliberately set to something the hardware cannot be: if this came out
    // the other end, the decoder took the setting rather than the machine.
    harness.settings.setSynthSampleRate(48000);

    const auto path = writeFixture("sc55.mid", tinyMidi());
    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 64000);

    REQUIRE_FALSE(decoded.samples.empty());
    INFO("hardware rate " << decoded.properties.format.sampleRate);
    CHECK(decoded.properties.encoding == "Roland SC-55mk2");
    CHECK(decoded.properties.format.channels == 2);
    CHECK(decoded.properties.format.sampleRate != 48000.0);
    CHECK(decoded.properties.format.sampleRate > 8000.0);

    // A machine that booted and played, rather than one that booted.
    CHECK(peak(decoded.samples) > kAudible);
}

TEST_CASE("a configured SC-55 with no ROMs plays on the OPL instead",
          "[midi][sc55]") {
    Harness harness;
    harness.settings.setMidiPlugin("NukeSc55");
    harness.settings.setMidiRomPath("");

    const auto    path = writeFixture("sc55-missing.mid", tinyMidi());
    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    // Still plays, which is the point -- refusing would mean a `.mid` that
    // cannot be opened at all until five files are found.
    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(peak(decoded.samples) > kAudible);

    // And says what actually made the sound, which is the only way anyone could
    // tell that the setting did not get what it asked for.
    CHECK(decoded.properties.encoding == "Nuked OPL3 (DMX)");
}

// ---------------------------------------------------------------------------
// The front panel
// ---------------------------------------------------------------------------
// Stage 3b: the LCD state comes out of the emulator positioned in the rendered
// stream, so a display can be driven from it. No pixels yet -- what is at risk
// here is the timing, and the timing can be settled without drawing anything.

namespace {

/// Boots a machine, or skips. Booting is seven seconds of emulated time, so
/// these share nothing and each pays for it once.
[[nodiscard]] bool boot(codecs::Sc55Synth& synth) {
    if (!kHaveRoms || !fs::exists(romPath())) {
        return false;
    }
    const auto roms = codecs::loadSc55Roms(romPath());
    return roms.has_value() && synth.open(*roms);
}

/// Something for the panel to react to: a program change puts an instrument
/// name on the display, which is the whole point of watching it.
void playSomething(codecs::Sc55Synth& synth) {
    synth.write(0x0000C0u);        // program change, channel 0, piano
    synth.write(0x64'3C'90u);      // note on, middle C, velocity 100
}

}  // namespace

TEST_CASE("the panel is not captured unless something is watching",
          "[midi][sc55]") {
    codecs::Sc55Synth synth;
    if (!boot(synth)) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    std::vector<std::int16_t> audio(2 * 8192);
    playSomething(synth);
    synth.render(audio.data(), 8192);

    // Capture is off by default, and off means nothing is queued rather than
    // queued and thrown away -- a player with no panel on screen should not be
    // paying for one.
    CHECK(synth.takeLcdFrames().empty());

    synth.setCaptureLcd(true);
    playSomething(synth);
    synth.render(audio.data(), 8192);
    CHECK_FALSE(synth.takeLcdFrames().empty());
}

TEST_CASE("panel states are positioned in the stream, not since boot",
          "[midi][sc55]") {
    codecs::Sc55Synth synth;
    if (!boot(synth)) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }
    synth.setCaptureLcd(true);

    const auto rate   = static_cast<std::uint64_t>(synth.sampleRate());
    const auto frames = static_cast<std::size_t>(rate);  // one second

    std::vector<std::int16_t> audio(frames * 2);
    playSomething(synth);
    synth.render(audio.data(), frames);

    const auto captured = synth.takeLcdFrames();
    REQUIRE_FALSE(captured.empty());

    // The one that matters. api.h calls the timestamp "absolute elapsed since
    // boot", and booting is seven seconds -- so a reading of api.h rather than
    // of mcu.cpp puts every frame seven seconds into a stream that is one
    // second long. Nothing about that failure is visible except a panel that
    // never updates.
    for (const codecs::Sc55LcdFrame& frame : captured) {
        INFO("frame at sample " << frame.samplePosition << " of " << frames);
        CHECK(frame.samplePosition <= frames);
        CHECK_FALSE(frame.state.empty());
    }

    // Ordered, because a display walks them forwards.
    for (std::size_t i = 1; i < captured.size(); ++i) {
        CHECK(captured[i].samplePosition >= captured[i - 1].samplePosition);
    }
}

TEST_CASE("panel positions do not depend on how the audio was chunked",
          "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    // The same second of music, rendered once in a single call and once in a
    // hundred small ones. If the positions came from the caller's chunking
    // rather than from the machine's own counter, these would disagree -- and
    // the panel would drift by the buffer size, which changes with the output
    // device.
    const auto renderPositions =
        [](std::size_t chunk) -> std::vector<std::uint64_t> {
        codecs::Sc55Synth synth;
        if (!boot(synth)) {
            return {};
        }
        synth.setCaptureLcd(true);
        const auto frames = static_cast<std::size_t>(synth.sampleRate());

        std::vector<std::int16_t> audio(chunk * 2);
        playSomething(synth);
        for (std::size_t done = 0; done < frames; done += chunk) {
            synth.render(audio.data(), std::min(chunk, frames - done));
        }

        std::vector<std::uint64_t> positions;
        for (const codecs::Sc55LcdFrame& frame : synth.takeLcdFrames()) {
            positions.push_back(frame.samplePosition);
        }
        return positions;
    };

    const auto whole = renderPositions(static_cast<std::size_t>(66207));
    const auto split = renderPositions(662);
    REQUIRE_FALSE(whole.empty());
    REQUIRE_FALSE(split.empty());
    CHECK(whole == split);
}

TEST_CASE("decoding an SC-55 track feeds the panel, positioned in the track",
          "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    PanelFeed& feed = PanelFeed::instance();
    feed.clear();
    feed.setWanted(true);

    Harness harness;
    harness.settings.setMidiPlugin("NukeSc55");
    harness.settings.setMidiRomPath(romPath().string());

    const auto path = writeFixture("sc55-panel.mid", tinyMidi());
    const Url  url  = Url::fromLocalPath(path);
    feed.setAudibleTrack(url);

    const Decoded decoded = decode(harness.registry, url, 4 * 64000);
    REQUIRE_FALSE(decoded.samples.empty());

    // Everything the panel did during a one-second track, drained as if the
    // speaker had reached the end of it.
    const auto frames = feed.take(60.0);
    feed.setWanted(false);
    feed.clear();

    REQUIRE_FALSE(frames.empty());
    for (const PanelFrame& frame : frames) {
        INFO("panel frame at " << frame.seconds << "s");
        // Inside the track, not seven seconds into it -- the same api.h trap the
        // synthesiser's own tests cover, checked again here because this is the
        // path a display actually takes and the two could drift apart.
        CHECK(frame.seconds >= 0.0);
        CHECK(frame.seconds <= 5.0);
        CHECK_FALSE(frame.state.empty());
    }
}

TEST_CASE("nothing feeds the panel when nothing is displaying it",
          "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    PanelFeed& feed = PanelFeed::instance();
    feed.clear();
    feed.setWanted(false);

    Harness harness;
    harness.settings.setMidiPlugin("NukeSc55");
    harness.settings.setMidiRomPath(romPath().string());

    const auto path = writeFixture("sc55-nopanel.mid", tinyMidi());
    const Url  url  = Url::fromLocalPath(path);
    feed.setAudibleTrack(url);

    const Decoded decoded = decode(harness.registry, url, 4 * 64000);
    REQUIRE_FALSE(decoded.samples.empty());

    // The emulator compares the panel against its previous state on every
    // sample it renders when capture is on. A player with no panel on screen
    // should not be paying for that, and "not paying" is only observable as
    // nothing arriving.
    CHECK(feed.take(60.0).empty());
    feed.clear();
}

TEST_CASE("opening the panel part-way through a track starts feeding it",
          "[midi][sc55]") {
    if (!kHaveRoms || !fs::exists(romPath())) {
        SKIP("no ROMs: configure with -DXPCOG_SC55_ROMS=<folder or archive>");
    }

    PanelFeed& feed = PanelFeed::instance();
    feed.clear();
    feed.setWanted(false);

    Harness harness;
    harness.settings.setMidiPlugin("NukeSc55");
    harness.settings.setMidiRomPath(romPath().string());

    const auto path = writeFixture("sc55-late.mid", tinyMidi());
    const Url  url  = Url::fromLocalPath(path);
    feed.setAudibleTrack(url);

    PluginRegistry::OpenResult opened = harness.registry.open(url);
    REQUIRE(opened);

    // Play a little with the display closed, which is the ordinary case: a
    // panel nobody has opened costs the emulator nothing.
    AudioChunk chunk;
    REQUIRE(opened.decoder->readAudio(chunk));
    CHECK(feed.take(60.0).empty());

    // Now open it. The decoder asks per read rather than latching at open, so
    // this has to start working without waiting for the next track -- which is
    // the whole reason it is asked per read.
    feed.setWanted(true);
    for (int i = 0; i < 200 && !feed.producing(); ++i) {
        if (!opened.decoder->readAudio(chunk)) {
            break;
        }
    }

    CHECK(feed.producing());
    CHECK_FALSE(feed.take(60.0).empty());

    feed.setWanted(false);
    feed.clear();
}
