// Stage 1 of the MIDI port: that a MIDI file makes a sound.
//
// The sequencer landed on its own and was tested on its own, which left exactly
// one thing unproven and it is the thing that matters -- that the events reach a
// synthesiser at the right samples and come back as audio. Nuked OPL3 is that
// synthesiser (codecs/midi/OplSynth.hpp), and it is first because it is the one
// of the three that needs nothing the listener has to supply: if a `.mid` is
// silent here, nothing else can be blamed.
//
// Three failures are worth naming, because each of them plays perfectly well
// while being wrong:
//
//   The fragment ignored. Every subsong opens, reports a duration, and renders
//   -- the first one, once per playlist entry. This is what the QSF core was
//   caught doing, and only differing audio catches it.
//
//   The setting ignored. `midiPlugin` chooses what the file sounds like; a
//   decoder that never reads it still plays, on whichever synthesiser it
//   happened to construct. Only the two drivers differing catches that.
//
//   The seek approximate. An FM chip has envelopes in flight, so a seek that
//   does not replay the sequence lands in a chip state that never existed.
//   Only comparing against a straight decode catches it.

#include "MidiFixtures.hpp"

#include "xpcog/core/AudioChunk.hpp"
#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Url.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::testing;

namespace {

/// A registry of its own, so a settings object can be attached to it without
/// the shared one in the other codec tests carrying those settings around.
struct Harness {
    std::unique_ptr<ISettingsStore> store = makeMemorySettingsStore();
    Settings                        settings{*store};
    PluginRegistry                  registry;

    Harness() {
        registerAllCodecs(registry);
        registry.setSettings(&settings);
    }
};

fs::path fixtureDir() {
    static const fs::path dir = [] {
        auto path = fs::temp_directory_path() / "xpcog-midi-tests";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

fs::path writeFixture(const std::string& name,
                      const std::vector<std::uint8_t>& bytes) {
    const fs::path path = fixtureDir() / name;
    std::FILE*     file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return path;
}

struct Decoded {
    std::vector<std::int16_t> samples;
    TrackProperties           properties;
    MetadataMap               tags;
};

[[nodiscard]] Decoded decode(const PluginRegistry& registry, const Url& url,
                             std::size_t limitSamples) {
    Decoded out;
    PluginRegistry::OpenResult opened = registry.open(url);
    if (!opened) {
        return out;
    }
    out.properties = opened.decoder->properties();
    out.tags       = opened.decoder->metadata();

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

/// A silent decode and a decode of silence are the same thing, so this is what
/// separates "the chip ran" from "the chip played". The threshold is low
/// because a single FM note at velocity 100 is not loud, and high enough that
/// the resampler's ringing on a genuinely silent stream cannot reach it.
constexpr int kAudible = 500;

constexpr std::size_t kTwoSeconds = 2 * 44100 * 2;

}  // namespace

TEST_CASE("the MIDI decoder is registered for the formats it can parse", "[midi]") {
    Harness harness;
    for (const char* extension : {"mid", "midi", "rmi", "mids", "hmi", "hmp",
                                  "xmi", "mus", "lds", "kar"}) {
        INFO(extension);
        CHECK(harness.registry.isPlayableExtension(extension));
    }
}

TEST_CASE("a MIDI file renders audio rather than silence", "[midi]") {
    Harness    harness;
    const auto path = writeFixture("tiny.mid", tinyMidi());

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), kTwoSeconds);

    REQUIRE_FALSE(decoded.samples.empty());
    CHECK(decoded.properties.codec == "MIDI");
    CHECK(decoded.properties.format.sampleRate == 44100.0);
    CHECK(decoded.properties.format.channels == 2);

    // One second of held middle C at 120 bpm, so the whole track is one note.
    CHECK(decoded.properties.totalFrames == 44100);
    CHECK(decoded.samples.size() == 44100 * 2);
    CHECK(peak(decoded.samples) > kAudible);

    // The tempo map read, and the title with it -- both already covered by the
    // container tests, checked again here because this is the path a listener
    // actually takes and the two could diverge without either noticing.
    CHECK(decoded.tags.first("title") == "Tiny");
}

TEST_CASE("the chip is quiet before the first note reaches it", "[midi]") {
    Harness    harness;
    const auto path = writeFixture("tiny-quiet.mid", tinyMidi());

    const Decoded decoded =
        decode(harness.registry, Url::fromLocalPath(path), kTwoSeconds);
    REQUIRE(decoded.samples.size() > 44100);

    // opl3class queues register writes 50 ms ahead of the chip's clock, so the
    // note-on at t=0 does not sound until roughly there. That delay is Cog's
    // too and is why this checks the first 40 ms rather than the first sample:
    // what would be wrong is not the latency but audio arriving before any
    // event has been delivered at all, which is what a decoder that rendered
    // the whole sequence up front and then handed out slices would produce.
    constexpr std::size_t kFortyMilliseconds = 44100 * 40 / 1000;
    const std::vector<std::int16_t> head(
        decoded.samples.begin(),
        decoded.samples.begin() + kFortyMilliseconds * 2);
    CHECK(peak(head) == 0);
}

TEST_CASE("each subsong of a format-2 file plays its own sequence", "[midi]") {
    Harness    harness;
    const auto path = writeFixture("two.mid", formatTwoMidi());
    const Url  url  = Url::fromLocalPath(path);

    // The container offers one entry per sequence, numbered from zero as Cog
    // numbers them.
    const std::vector<Url> expanded = harness.registry.expandContainer(url);
    REQUIRE(expanded.size() == 2);
    CHECK(expanded[0].fragment() == "0");
    CHECK(expanded[1].fragment() == "1");

    const Decoded first  = decode(harness.registry, expanded[0], kTwoSeconds * 4);
    const Decoded second = decode(harness.registry, expanded[1], kTwoSeconds * 4);

    // Different lengths, because the fixture's two sequences are one and two
    // seconds. A decoder that accepted the fragment and ignored it would report
    // the same duration twice and look entirely healthy doing it.
    CHECK(first.properties.totalFrames == 44100);
    CHECK(second.properties.totalFrames == 88200);
    CHECK(first.tags.first("title") == "First");
    CHECK(second.tags.first("title") == "Second");

    // And both are audible, which is what says the second one is a rendered
    // sequence rather than an empty one padded to length.
    CHECK(peak(first.samples) > kAudible);
    CHECK(peak(second.samples) > kAudible);
}

TEST_CASE("the midiPlugin setting reaches the synthesiser", "[midi]") {
    const auto path = writeFixture("tiny-synth.mid", tinyMidi());
    const Url  url  = Url::fromLocalPath(path);

    Harness doom;
    doom.settings.setMidiPlugin("DOOM0");
    const Decoded withDoom = decode(doom.registry, url, kTwoSeconds);

    Harness dmxopl;
    dmxopl.settings.setMidiPlugin("DOOM5");
    const Decoded withOtherBank = decode(dmxopl.registry, url, kTwoSeconds);

    Harness general;
    general.settings.setMidiPlugin("OPL3W0");
    const Decoded withGeneralMidi = decode(general.registry, url, kTwoSeconds);

    REQUIRE(withDoom.samples.size() == withOtherBank.samples.size());
    REQUIRE(withDoom.samples.size() == withGeneralMidi.samples.size());
    for (const Decoded* decoded : {&withDoom, &withOtherBank, &withGeneralMidi}) {
        CHECK(peak(decoded->samples) > kAudible);
    }

    // Three renderings of one note. The same chip in all three, so what differs
    // is only the operator settings the driver chose for program 0 -- which is
    // the whole of what the setting decides, and is inaudible in a test that
    // only asks whether something came out.
    CHECK(withDoom.samples != withOtherBank.samples);
    CHECK(withDoom.samples != withGeneralMidi.samples);

    // An unrecognised name is not a reason to refuse the file: a settings file
    // carried over from a macOS Cog names an AudioUnit that does not exist
    // here, and the file should still play on the default.
    Harness stranger;
    stranger.settings.setMidiPlugin("dls ");
    const Decoded withFallback = decode(stranger.registry, url, kTwoSeconds);
    CHECK(withFallback.samples == withDoom.samples);
}

TEST_CASE("seeking lands where a straight decode would have", "[midi]") {
    Harness    harness;
    const auto path = writeFixture("tiny-seek.mid", tinyMidi());
    const Url  url  = Url::fromLocalPath(path);

    const Decoded straight = decode(harness.registry, url, kTwoSeconds);
    REQUIRE(straight.samples.size() == 44100 * 2);

    PluginRegistry::OpenResult opened = harness.registry.open(url);
    REQUIRE(opened);

    constexpr std::int64_t kTarget = 22050;  // half a second in
    REQUIRE(opened.decoder->seek(kTarget) == kTarget);

    std::vector<std::int16_t> seeked;
    AudioChunk                chunk;
    while (opened.decoder->readAudio(chunk)) {
        const std::size_t frames = chunk.frameCount();
        if (frames == 0) {
            break;
        }
        const std::size_t at = seeked.size();
        seeked.resize(at + frames * 2);
        std::memcpy(seeked.data() + at, chunk.bytes().data(),
                    frames * 2 * sizeof(std::int16_t));
    }

    // Sample for sample, not approximately. The seek replays the sequence from
    // the start and throws the audio away, so there is no reason for a single
    // sample to differ -- and if one does, the chip was resumed from a state it
    // could not have reached by playing.
    REQUIRE(seeked.size() == straight.samples.size() - kTarget * 2);
    CHECK(seeked == std::vector<std::int16_t>(
                        straight.samples.begin() + kTarget * 2,
                        straight.samples.end()));
}

TEST_CASE("the corpus plays, not merely parses", "[midi][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_MIDI_CORPUS=<path> to run this");
    }

    Harness harness;
    // Two formats, because the drivers were written for different worlds: `mid`
    // is what the General MIDI driver expects, `mus` is Doom's own and is what
    // the DMX banks exist for. A synth that only managed one of them would be
    // half a stage.
    for (const std::string_view extension : {"mid", "mus"}) {
        const auto files = findByExtension(extension, 12);
        if (files.empty()) {
            continue;
        }

        std::size_t audible = 0;
        for (const fs::path& file : files) {
            INFO(file.filename().string());
            // Five seconds each: enough for any of these to have started, and
            // short enough that twenty-four renderings are not the test run.
            const Decoded decoded = decode(harness.registry,
                                           Url::fromLocalPath(file), 5 * 44100 * 2);
            if (!decoded.samples.empty() && peak(decoded.samples) > kAudible) {
                ++audible;
            }
        }

        INFO(extension << ": " << audible << " of " << files.size() << " audible");
        // Not all of them. A corpus this size holds truncated files, files that
        // open with a long silence, and files that are only a SysEx dump --
        // refusing or rendering nothing for those is correct. What would be a
        // failure is a whole format that never makes a sound.
        CHECK(audible > files.size() / 2);
    }
}
