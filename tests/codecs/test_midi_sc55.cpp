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

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"

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
