// Stage 0 of the MIDI port: that the sequencer parses, and what it parses.
//
// No synth exists yet, so nothing here listens to anything. What it checks is
// the layer everything else will stand on -- that a file becomes a container
// with a plausible duration, that the formats beyond Standard MIDI actually
// reach their own processors, and that a file which is not MIDI is refused.
//
// Two fixtures rather than one kind. A handful are built here byte by byte, so
// the basics run everywhere including CI; the breadth comes from an opt-in
// corpus (`-DXPCOG_MIDI_CORPUS=<path>`), because the interesting formats --
// HMI, Doom's MUS, Loudness LDS -- are not things a test can synthesise
// convincingly and are not things anyone can commit.

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
namespace fs = std::filesystem;

namespace {

/// A minimal type-0 Standard MIDI file: one track, one note, one end-of-track.
/// Written out by hand because every byte of it is a thing the parser has to
/// get right, and because it makes the "is it MIDI at all" test meaningful.
std::vector<std::uint8_t> tinyMidi() {
    const auto be16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x >> 8));
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    };
    const auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            v.push_back(static_cast<std::uint8_t>((x >> shift) & 0xFF));
        }
    };

    std::vector<std::uint8_t> track;
    // delta 0: tempo 500000 us/quarter, which is 120 bpm
    track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
    // delta 0: track name
    const std::string name = "Tiny";
    track.insert(track.end(), {0x00, 0xFF, 0x03,
                               static_cast<std::uint8_t>(name.size())});
    track.insert(track.end(), name.begin(), name.end());
    // delta 0: note on, middle C, velocity 100
    track.insert(track.end(), {0x00, 0x90, 0x3C, 0x64});
    // delta 960 (two quarters at 480 ppqn, so one second at 120 bpm): note off
    track.insert(track.end(), {0x87, 0x40, 0x80, 0x3C, 0x00});
    // delta 0: end of track
    track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(out, 6);
    be16(out, 0);    // format 0
    be16(out, 1);    // one track
    be16(out, 480);  // ticks per quarter note
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    be32(out, static_cast<std::uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::vector<std::uint8_t> bytes;
    std::FILE*                f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return bytes;
    }
    std::uint8_t buffer[16384];
    std::size_t  got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    std::fclose(f);
    return bytes;
}

#ifdef XPCOG_MIDI_CORPUS
constexpr bool kHaveCorpus = true;
[[nodiscard]] fs::path corpusRoot() { return fs::path{XPCOG_MIDI_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] fs::path corpusRoot() { return {}; }
#endif

/// Up to `limit` files with the given extension, so a sweep over a corpus of
/// two hundred thousand does not become the whole test run.
std::vector<fs::path> findByExtension(std::string_view extension, std::size_t limit) {
    std::vector<fs::path> found;
    if (!kHaveCorpus || !fs::exists(corpusRoot())) {
        return found;
    }
    std::error_code error;
    for (fs::recursive_directory_iterator it{
             corpusRoot(), fs::directory_options::skip_permission_denied, error};
         it != fs::recursive_directory_iterator{}; it.increment(error)) {
        if (error) {
            break;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!ext.empty() && ext.substr(1) == extension) {
            found.push_back(it->path());
            if (found.size() >= limit) {
                break;
            }
        }
    }
    return found;
}

}  // namespace

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

    const auto events = file.stream(0, 44100.0);
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
