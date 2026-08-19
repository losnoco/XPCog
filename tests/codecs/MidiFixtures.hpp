// Hand-built MIDI files, and the opt-in corpus.
//
// Shared by every MIDI stage: the container tests parse these, the playback
// tests render them, and the two synthesisers still to land (docs/MIDI.md) will
// want exactly the same ones. Written out byte by byte because every byte is
// something the parser has to get right, and because a fixture a test can build
// runs on CI where a corpus never will.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::testing {

namespace fs = std::filesystem;

/// A minimal type-0 Standard MIDI file: one track, one note, one end-of-track.
/// Written out by hand because every byte of it is a thing the parser has to
/// get right, and because it makes the "is it MIDI at all" test meaningful.
inline std::vector<std::uint8_t> tinyMidi() {
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

/// A format-2 file: two independent sequences, of different lengths.
///
/// Format 2 is the one case where a Standard MIDI file is several songs rather
/// than several parts of one, and it is what XMI is mapped onto -- so this is
/// the fixture that makes subsongs real without needing an XMI to hand.
inline std::vector<std::uint8_t> formatTwoMidi() {
    const auto be16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x >> 8));
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    };
    const auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            v.push_back(static_cast<std::uint8_t>((x >> shift) & 0xFF));
        }
    };

    // One sequence: a note held for `quarters` quarter-notes, named `name`.
    const auto sequence = [](const std::string& name, int quarters) {
        std::vector<std::uint8_t> track;
        track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
        track.insert(track.end(), {0x00, 0xFF, 0x03,
                                   static_cast<std::uint8_t>(name.size())});
        track.insert(track.end(), name.begin(), name.end());
        track.insert(track.end(), {0x00, 0x90, 0x3C, 0x64});
        // A variable-length delta of quarters * 480 ticks.
        const std::uint32_t delta = static_cast<std::uint32_t>(quarters) * 480U;
        std::vector<std::uint8_t> vlq;
        std::uint32_t             value = delta;
        vlq.push_back(static_cast<std::uint8_t>(value & 0x7F));
        while ((value >>= 7) != 0) {
            vlq.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        }
        track.insert(track.end(), vlq.rbegin(), vlq.rend());
        track.insert(track.end(), {0x80, 0x3C, 0x00});
        track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});
        return track;
    };

    const auto first  = sequence("First", 2);   // one second at 120 bpm
    const auto second = sequence("Second", 4);  // two seconds

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(out, 6);
    be16(out, 2);    // format 2: independent sequences
    be16(out, 2);    // two of them
    be16(out, 480);
    for (const auto& track : {first, second}) {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        be32(out, static_cast<std::uint32_t>(track.size()));
        out.insert(out.end(), track.begin(), track.end());
    }
    return out;
}

inline std::vector<std::uint8_t> readFile(const fs::path& path) {
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
[[nodiscard]] inline fs::path corpusRoot() { return fs::path{XPCOG_MIDI_CORPUS}; }
#else
constexpr bool kHaveCorpus = false;
[[nodiscard]] inline fs::path corpusRoot() { return {}; }
#endif

/// Up to `limit` files with the given extension, so a sweep over a corpus of
/// two hundred thousand does not become the whole test run.
inline std::vector<fs::path> findByExtension(std::string_view extension, std::size_t limit) {
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

}  // namespace xpcog::testing
