// The sequencer: that a MIDI file parses, and what it parses into.
//
// Nothing here listens to anything -- rendering is test_midi_playback.cpp's
// half. What this checks is the layer everything else stands on: that a file
// becomes a container with a plausible duration, that the formats beyond
// Standard MIDI actually reach their own processors, and that a file which is
// not MIDI is refused.
//
// Two kinds of fixture. A handful are built byte by byte in MidiFixtures.hpp,
// so the basics run everywhere including CI; the breadth comes from an opt-in
// corpus (`-DXPCOG_MIDI_CORPUS=<path>`), because the interesting formats --
// HMI, Doom's MUS, Loudness LDS -- are not things a test can synthesise
// convincingly and are not things anyone can commit.

#include "MidiFixtures.hpp"

#include "midi/MidiFile.hpp"

#include "xpcog/core/MetadataMap.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::testing;
namespace fs = std::filesystem;

TEST_CASE("a Standard MIDI file parses to one subsong of a known length", "[midi]") {
    codecs::MidiFile file;
    REQUIRE(file.parse(tinyMidi(), "mid"));
    CHECK(file.valid());
    REQUIRE(file.subsongCount() == 1);

    // One second: 960 ticks at 480 per quarter is two quarters, and 120 bpm
    // makes a quarter half a second. If the tempo map were ignored the parser
    // would answer with the default 120 bpm and agree by accident, so the tempo
    // event above states exactly that default on purpose -- what this checks is
    // the tick arithmetic, and the corpus checks the rest.
    CHECK(file.duration(0) == Catch::Approx(1.0).margin(0.05));

    CHECK(file.metadata(0).first("title") == "Tiny");
}

TEST_CASE("the event stream comes out in order and in samples", "[midi]") {
    codecs::MidiFile file;
    REQUIRE(file.parse(tinyMidi(), "mid"));

    const auto events = file.stream(0, 44100.0).events;
    REQUIRE(events.size() >= 2);

    // Non-decreasing, which is the one property a synth may rely on.
    for (std::size_t i = 1; i < events.size(); ++i) {
        CHECK(events[i].timestampSamples >= events[i - 1].timestampSamples);
    }

    // And the last of them lands where the duration said it would, which is
    // what proves the seconds-to-samples conversion is applied rather than the
    // library's own seconds being passed through as if they were samples.
    CHECK(static_cast<double>(events.back().timestampSamples) ==
          Catch::Approx(44100.0).margin(2205.0));
}

TEST_CASE("a format-2 file is several songs, not several parts", "[midi]") {
    codecs::MidiFile file;
    REQUIRE(file.parse(formatTwoMidi(), "mid"));

    // Two, because format 2 says the tracks are independent. Everything else --
    // format 0 and format 1 -- is one song however many tracks it has, and
    // midi_container draws that line itself: every path that is not form 2
    // writes to channel mask zero, so the count falls out of the parse rather
    // than out of a rule stated twice.
    REQUIRE(file.subsongCount() == 2);

    // Different lengths, which is what proves the subsong index reaches
    // get_timestamp_end rather than being accepted and ignored. A decoder that
    // dropped it would report the first song's duration for both and look
    // entirely healthy doing it -- the failure the QSF core was caught by.
    CHECK(file.duration(0) == Catch::Approx(1.0).margin(0.05));
    CHECK(file.duration(1) == Catch::Approx(2.0).margin(0.05));

    // And the same for metadata and the event stream, so all three accessors
    // are known to address the song they were asked for.
    CHECK(file.metadata(0).first("title") == "First");
    CHECK(file.metadata(1).first("title") == "Second");
    CHECK(file.stream(1, 44100.0).events.back().timestampSamples >
          file.stream(0, 44100.0).events.back().timestampSamples);
}

TEST_CASE("a file that is not MIDI is refused", "[midi]") {
    codecs::MidiFile file;
    const std::vector<std::uint8_t> notMidi(512, 0x42);
    CHECK_FALSE(file.parse(notMidi, "mid"));
    CHECK_FALSE(file.valid());
    CHECK(file.subsongCount() == 0);
    CHECK(file.duration(0) == 0.0);
}

TEST_CASE("the corpus parses across every format it holds", "[midi][corpus]") {
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        SKIP("no corpus: configure with -DXPCOG_MIDI_CORPUS=<path> to run this");
    }

    // One processor per row, and the point of the table is that a `.mus` must
    // reach midi_processor_mus and not be quietly refused by the Standard MIDI
    // one. A format absent from a given corpus skips rather than fails.
    struct Format {
        const char* extension;
        const char* what;
    };
    constexpr Format kFormats[] = {
        {"mid", "Standard MIDI"},   {"midi", "Standard MIDI"},
        {"rmi", "RIFF MIDI"},       {"mids", "RIFF MIDI, Microsoft's variant"},
        {"mus", "Doom"},            {"hmi", "Human Machine Interfaces"},
        {"hmp", "HMI's later one"}, {"xmi", "Miles/Origin"},
        {"lds", "Loudness"},
    };

    std::size_t formatsSeen = 0;
    for (const Format& format : kFormats) {
        const auto files = findByExtension(format.extension, 25);
        if (files.empty()) {
            continue;
        }
        ++formatsSeen;
        INFO("format: " << format.what << " (." << format.extension << ")");

        std::size_t parsed = 0;
        for (const fs::path& path : files) {
            const auto bytes = readFile(path);
            if (bytes.empty()) {
                continue;
            }
            codecs::MidiFile file;
            if (!file.parse(bytes, format.extension)) {
                continue;
            }
            ++parsed;
            // A sequence with no events is not a parse, it is a shrug.
            CHECK(file.subsongCount() >= 1);
            CHECK(file.duration(0) > 0.0);
        }

        // Not all of them: a corpus of this size holds truncated and misnamed
        // files, and refusing those is correct behaviour rather than a failure.
        // What would be a failure is a format whose processor never runs.
        INFO("parsed " << parsed << " of " << files.size());
        CHECK(parsed > files.size() / 2);
    }

    CHECK(formatsSeen > 0);
}

TEST_CASE("an RMID hands back the bank it carries", "[midi]") {
    // The point of RMID: the file arrives with the instruments its author
    // wrote it for, so it sounds the same on a machine that has never been
    // configured. Cog gets this for free because it feeds the file to
    // SpessaSynth's own loader, which auto-loads the embedded bank; this
    // reads the container itself, so it has to ask.
    const std::vector<std::uint8_t> bank = fakeSoundBank();

    codecs::MidiFile file;
    REQUIRE(file.parse(rmidWithBank(tinyMidi(), bank, 8), "rmi"));
    CHECK(file.valid());

    // Still a MIDI file underneath the wrapper.
    REQUIRE(file.subsongCount() == 1);
    CHECK(file.duration(0) == Catch::Approx(1.0).margin(0.05));

    const auto embedded = file.embeddedBank();
    REQUIRE(embedded.has_value());

    // Verbatim, header included: what comes back has to be loadable as a file
    // in its own right, and an SF2 that has lost its `RIFF` is not one.
    CHECK(embedded->bytes == bank);

    // And the offset the file stated, not one guessed by scanning. A bank
    // written to sit at MSB 8 plays the wrong instruments at MSB 0, and both
    // answers are plausible-looking numbers.
    CHECK(embedded->bankOffset == 8);
}

TEST_CASE("a MIDI file that carries no bank says so", "[midi]") {
    // The ordinary case, and it must be distinguishable from a bank of zero
    // bytes: an empty optional sends the decoder on to the companion bank and
    // then to the configured one, while an empty bank would be loaded and fail.
    codecs::MidiFile plain;
    REQUIRE(plain.parse(tinyMidi(), "mid"));
    CHECK_FALSE(plain.embeddedBank().has_value());

    // An RMID with no nested RIFF is the other half of it -- the wrapper alone
    // does not imply a bank.
    codecs::MidiFile wrapped;
    REQUIRE(wrapped.parse(rmidWithBank(tinyMidi(), {}, 0), "rmi"));
    CHECK(wrapped.valid());
    CHECK_FALSE(wrapped.embeddedBank().has_value());
}

TEST_CASE("a sequence's own reset says which dialect it is", "[midi]") {
    // What this decides is which of the two shipped banks plays: the XG bank,
    // or the map that puts a GS file's instruments back where it expects them.
    // Asked of the file, because the file is what knows.
    codecs::MidiFile plain;
    REQUIRE(plain.parse(tinyMidi(), "mid"));
    CHECK_FALSE(plain.dialect(0).gs);
    CHECK_FALSE(plain.dialect(0).gm2);
    CHECK_FALSE(plain.dialect(0).any());

    codecs::MidiFile gs;
    REQUIRE(gs.parse(gsResetMidi(), "mid"));
    CHECK(gs.dialect(0).gs);
    CHECK_FALSE(gs.dialect(0).gm2);
    CHECK(gs.dialect(0).any());

    // A subsong that does not exist is not a dialect. Cheap to state, and the
    // decoder asks about whichever subsong the URL's fragment named.
    CHECK_FALSE(gs.dialect(99).any());
}
