// Stage 2 of the MIDI port: SpessaSynth, playing a bank of real instruments.
//
// The bank is the asset problem again, and a worse one than the SC-55's: the
// one this was written against is 1.3 GB. So the cases that need sound are
// opt-in behind `XPCOG_SOUNDFONT` -- point it at any .sf2, .sf3, .dls or
// .sflist -- and skip without it, the same shape the ROM and corpus tests use.
//
// What is worth testing here is not "does a SoundFont sound like instruments".
// It is the three decisions around it that are ours rather than the engine's:
//
//   Which bank a file plays on. A bank sitting beside the file wins over the
//   configured one, because a game rip that ships its instruments is asking to
//   be played with them. Cog's rule, and three name shapes to get right.
//
//   What happens when there is no bank at all. The engine cannot make a sound
//   without one, so the file falls back to the OPL rather than opening into
//   silence -- and the track properties have to say so, since that is the only
//   way anyone could tell.
//
//   That the setting still means what it says. Unlike the SC-55, this
//   synthesiser renders at whatever rate it is asked for.

#include "MidiFixtures.hpp"

#include "midi/SoundFontSynth.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::testing;

namespace {

#ifdef XPCOG_SOUNDFONT
constexpr bool kHaveBank = true;
[[nodiscard]] fs::path bankPath() { return fs::path{XPCOG_SOUNDFONT}; }
#else
constexpr bool kHaveBank = false;
[[nodiscard]] fs::path bankPath() { return {}; }
#endif

[[nodiscard]] bool haveBank() { return kHaveBank && fs::exists(bankPath()); }

struct Harness {
    std::unique_ptr<ISettingsStore> store = makeMemorySettingsStore();
    Settings                        settings{*store};
    PluginRegistry                  registry;

    Harness() {
        registerAllCodecs(registry);
        registry.setSettings(&settings);
    }
};

/// A directory of this test's own, so a companion bank left beside one fixture
/// cannot be found beside another.
[[nodiscard]] fs::path scratch(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "xpcog-midi-soundfont" / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

fs::path writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return path;
}

/// The configured bank, made to appear beside a file.
///
/// A hard link first, because the bank this was written against is 1.3 GB and
/// nothing is worth copying that for. A link only works within one volume
/// though, and a bank on another drive than the temporary directory is the
/// ordinary case -- so a small bank is copied instead, and a large one on the
/// wrong volume skips the case rather than filling a disk.
[[nodiscard]] bool placeBankAs(const fs::path& target) {
    std::error_code error;
    fs::create_hard_link(bankPath(), target, error);
    if (!error) {
        return true;
    }
    constexpr std::uintmax_t kCopyLimit = 64U * 1024U * 1024U;
    const std::uintmax_t     size       = fs::file_size(bankPath(), error);
    if (error || size > kCopyLimit) {
        return false;
    }
    fs::copy_file(bankPath(), target, fs::copy_options::overwrite_existing, error);
    return !error;
}

struct Decoded {
    std::vector<float> samples;
    TrackProperties    properties;
};

[[nodiscard]] Decoded decode(const PluginRegistry& registry, const Url& url,
                             std::size_t limitSamples) {
    Decoded                    out;
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
                    frames * channels * sizeof(float));
    }
    return out;
}

[[nodiscard]] float peak(const std::vector<float>& samples) {
    float highest = 0.0F;
    for (const float sample : samples) {
        highest = std::max(highest, std::fabs(sample));
    }
    return highest;
}

/// Full scale is 1.0, so the threshold the other MIDI tests write as 500 of
/// 32768 is written as exactly that.
constexpr float kAudible = 500.0F / 32768.0F;

}  // namespace

// ---------------------------------------------------------------------------
// Which bank a file plays on
// ---------------------------------------------------------------------------
// Nothing here opens a bank, so none of it needs one: what is being tested is
// which name is looked for, and an empty file has a name.

TEST_CASE("a file's own bank is found under any of the three names",
          "[midi][soundfont]") {
    const fs::path dir = scratch("companion");
    const fs::path mid = writeFile(dir / "song.mid", tinyMidi());

    SECTION("nothing beside it") {
        CHECK_FALSE(codecs::findCompanionBank(mid).has_value());
    }

    SECTION("song.mid.sf2 -- the whole name, extension and all") {
        const fs::path bank = writeFile(dir / "song.mid.sf2", {});
        const auto     found = codecs::findCompanionBank(mid);
        REQUIRE(found.has_value());
        CHECK(*found == bank);
    }

    SECTION("song.sf2 -- the name without its extension") {
        const fs::path bank = writeFile(dir / "song.sf2", {});
        const auto     found = codecs::findCompanionBank(mid);
        REQUIRE(found.has_value());
        CHECK(*found == bank);
    }

    SECTION("companion/companion.sf2 -- one bank for the whole folder") {
        const fs::path bank = writeFile(dir / "companion.sf2", {});
        const auto     found = codecs::findCompanionBank(mid);
        REQUIRE(found.has_value());
        CHECK(*found == bank);
    }
}

TEST_CASE("a list of banks is preferred to a bank", "[midi][soundfont]") {
    const fs::path dir = scratch("list-first");
    const fs::path mid = writeFile(dir / "song.mid", tinyMidi());
    writeFile(dir / "song.sf2", {});
    const fs::path list = writeFile(dir / "song.sflist", {});

    // A folder holding both is one where somebody wrote the list: it says which
    // programs and channels each bank covers, and the bank alone says nothing.
    const auto found = codecs::findCompanionBank(mid);
    REQUIRE(found.has_value());
    CHECK(*found == list);
}

// ---------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------

TEST_CASE("SpessaSynth plays the configured bank", "[midi][soundfont]") {
    if (!haveBank()) {
        SKIP("no bank: configure with -DXPCOG_SOUNDFONT=<path to .sf2/.sf3>");
    }

    Harness harness;
    harness.settings.setMidiPlugin("Spessa");
    harness.settings.setSoundFontPath(pathToUtf8(bankPath()));
    // Unlike the SC-55, this one renders at whatever it is asked for.
    harness.settings.setSynthSampleRate(48000);

    const fs::path path =
        writeFile(scratch("configured") / "configured.mid", tinyMidi());
    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 48000);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.format.sampleRate == 48000.0);
    CHECK(decoded.properties.format.channels == 2);
    // Float out, because this synthesiser mixes in float and clipping it to
    // 16 bits here would be this decoder deciding where a bank distorts.
    CHECK(decoded.properties.format.format == SampleFormat::F32);
    CHECK(decoded.properties.encoding ==
          "SpessaSynth (" + pathToUtf8(bankPath().filename()) + ")");
    CHECK(peak(decoded.samples) > kAudible);
}

TEST_CASE("a file's own bank wins over the configured synthesiser",
          "[midi][soundfont]") {
    if (!haveBank()) {
        SKIP("no bank: configure with -DXPCOG_SOUNDFONT=<path to .sf2/.sf3>");
    }

    const fs::path dir  = scratch("beside");
    const fs::path path = writeFile(dir / "beside.mid", tinyMidi());
    const fs::path bank = dir / ("beside" + pathToUtf8(bankPath().extension()));
    if (!placeBankAs(bank)) {
        SKIP("the bank is on another volume and too large to copy");
    }

    Harness harness;
    // Asked for the OPL, and not given it: the file brought its own instruments.
    harness.settings.setMidiPlugin("DOOM0");
    harness.settings.setSoundFontPath("");

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.encoding ==
          "SpessaSynth (" + pathToUtf8(bank.filename()) + ")");
    CHECK(peak(decoded.samples) > kAudible);
}

TEST_CASE("an unconfigured player uses the bank XPCog ships",
          "[midi][soundfont]") {
    // This case used to be "SpessaSynth with no bank plays on the OPL instead",
    // and that was the right behaviour while there was no bank to play. There
    // is one now (assets/soundfonts), which is the whole reason `midiPlugin`
    // defaults to `Spessa`: a fresh install plays MIDI on real instruments
    // rather than on an FM chip, as Cog's does.
    Harness harness;  // nothing set: this is what a first run looks like

    const auto shipped = codecs::shippedBank(/*wantsGsMap=*/false);
    if (!shipped) {
        SKIP("this build has no bank staged beside the test binary");
    }

    const fs::path path = writeFile(scratch("shipped") / "shipped.mid", tinyMidi());
    const Decoded  decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.encoding ==
          "SpessaSynth (" + pathToUtf8(shipped->filename()) + ")");
    CHECK(peak(decoded.samples) > kAudible);
}

TEST_CASE("a GS sequence gets the map, a plain one gets the bank",
          "[midi][soundfont]") {
    // Two files shipped, and which one plays is asked of the sequence rather
    // than of the listener. The bank is an XG bank; a sequence that announced
    // itself as GS wants instruments at bank numbers the XG bank puts
    // elsewhere, and `tg300b.sflist.json` is the 246-entry map that puts them
    // back. Cog decides this the same way (MIDIDecoder.mm:263).
    if (!codecs::shippedBank(/*wantsGsMap=*/false)) {
        SKIP("this build has no bank staged beside the test binary");
    }
    const auto mapped = codecs::shippedBank(/*wantsGsMap=*/true);
    REQUIRE(mapped.has_value());
    if (pathToUtf8(mapped->extension()) != ".json") {
        SKIP("this build staged the bank but not the map");
    }

    const fs::path dir = scratch("dialect");
    Harness        harness;

    const fs::path plain = writeFile(dir / "plain.mid", tinyMidi());
    const Decoded  plainOut =
        decode(harness.registry, Url::fromLocalPath(plain), 44100);
    REQUIRE_FALSE(plainOut.samples.empty());
    CHECK(plainOut.properties.encoding ==
          "SpessaSynth (GeneralUserXG-SFeTest.sf3)");

    const fs::path gs = writeFile(dir / "gs.mid", gsResetMidi());
    const Decoded  gsOut = decode(harness.registry, Url::fromLocalPath(gs), 44100);
    REQUIRE_FALSE(gsOut.samples.empty());
    CHECK(gsOut.properties.encoding == "SpessaSynth (tg300b.sflist.json)");
}

TEST_CASE("a bank that is not a bank falls back rather than failing",
          "[midi][soundfont]") {
    Harness harness;
    harness.settings.setMidiPlugin("Spessa");

    // A MIDI file named as the bank. This is what a mistyped setting looks
    // like, and it must not take the file down with it.
    const fs::path dir  = scratch("not-a-bank");
    const fs::path fake = writeFile(dir / "not-a-bank.sf2", tinyMidi());
    harness.settings.setSoundFontPath(pathToUtf8(fake));

    const fs::path path = writeFile(dir / "song.mid", tinyMidi());
    const Decoded  decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.encoding == "Nuked OPL3 (DMX)");
}

TEST_CASE("seeking a SoundFont track lands where it was asked to",
          "[midi][soundfont]") {
    if (!haveBank()) {
        SKIP("no bank: configure with -DXPCOG_SOUNDFONT=<path to .sf2/.sf3>");
    }

    Harness harness;
    harness.settings.setMidiPlugin("Spessa");
    harness.settings.setSoundFontPath(pathToUtf8(bankPath()));

    const fs::path path = writeFile(scratch("seek") / "seek.mid", tinyMidi());
    PluginRegistry::OpenResult opened =
        harness.registry.open(Url::fromLocalPath(path));
    REQUIRE(opened);

    AudioChunk chunk;
    REQUIRE(opened.decoder->readAudio(chunk));

    // Backwards, which for this synthesiser is a system reset and a replay of
    // the collapsed state -- nothing is reloaded, because the bank is what took
    // the time to open and it has not changed. The fixture is exactly one
    // second long, so what is left is exactly half of it.
    constexpr std::int64_t kTarget = 22050;
    REQUIRE(opened.decoder->seek(kTarget) == kTarget);

    std::size_t frames = 0;
    while (opened.decoder->readAudio(chunk)) {
        frames += chunk.frameCount();
    }
    CHECK(frames == 44100 - kTarget);
}

// ---------------------------------------------------------------------------
// The bank inside the file
// ---------------------------------------------------------------------------
//
// These need a real bank but not a large one, so they build the smallest SF2
// that makes a sound rather than borrowing `XPCOG_SOUNDFONT` -- which is 1.35 GB
// on the machine this was written on, and would produce a 1.35 GB RMID that the
// decoder refuses before reaching anything worth testing. So unlike everything
// above, these two run everywhere.

TEST_CASE("an RMID plays on the bank it carries", "[midi][soundfont]") {
    const fs::path path = writeFile(scratch("embedded") / "embedded.rmi",
                                    rmidWithBank(tinyMidi(), tinySoundFont(), 0));

    Harness harness;
    // Asked for the OPL, given something else -- the same rule as a bank
    // sitting beside the file, and for the same reason: a bank inside the file
    // is part of the music rather than a preference.
    harness.settings.setMidiPlugin("DOOM0");
    harness.settings.setSoundFontPath("");

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.encoding == "SpessaSynth (embedded bank)");
    CHECK(peak(decoded.samples) > kAudible);
}

TEST_CASE("an embedded bank outranks one beside the file", "[midi][soundfont]") {
    // Both present at once, which is the only way to see which rule wins.
    const fs::path dir  = scratch("embedded-beats-beside");
    const fs::path path = writeFile(dir / "both.rmi",
                                    rmidWithBank(tinyMidi(), tinySoundFont(), 0));
    writeFile(dir / "both.sf2", tinySoundFont());

    Harness harness;
    harness.settings.setMidiPlugin("Spessa");

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    // Not "SpessaSynth (both.sf2)": the companion bank was found and set aside.
    CHECK(decoded.properties.encoding == "SpessaSynth (embedded bank)");
}

TEST_CASE("an RMID whose bank will not load still plays", "[midi][soundfont]") {
    // A bank that is RIFF-shaped and nothing else. The container hands it over,
    // the engine refuses it, and the file has to end up somewhere rather than
    // failing to open -- which is the fallback the decoder spells out.
    const fs::path path = writeFile(scratch("bad-bank") / "bad.rmi",
                                    rmidWithBank(tinyMidi(), fakeSoundBank(), 0));

    Harness harness;
    harness.settings.setMidiPlugin("DOOM0");
    harness.settings.setSoundFontPath("");

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), 4 * 44100);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.encoding != "SpessaSynth (embedded bank)");
    CHECK(peak(decoded.samples) > kAudible);
}
