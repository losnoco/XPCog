// Hand-built MIDI files, and the opt-in corpus.
//
// Shared by every MIDI stage: the container tests parse these, the playback
// tests render them, and the two synthesisers still to land (docs/MIDI.md) will
// want exactly the same ones. Written out byte by byte because every byte is
// something the parser has to get right, and because a fixture a test can build
// runs on CI where a corpus never will.

#pragma once

#include "../TestSignal.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <cmath>
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

/// A file for testing seeks: a program change near the start, then a note well
/// after it.
///
/// `withProgram` is what makes it a pair. Seeking past the program change and
/// rendering the note gives one sound when the change was replayed and another
/// when it was not, which is the only way to see from the outside whether a
/// seek restored the synthesiser's state or merely moved the cursor.
inline std::vector<std::uint8_t> programChangeMidi(bool withProgram) {
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
    // 120 bpm, so a quarter note is half a second.
    track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
    if (withProgram) {
        // Program 60, French horn -- far enough from program 0 in any bank
        // that the two cannot be confused.
        track.insert(track.end(), {0x00, 0xC0, 0x3C});
    }
    // delta 480 (one quarter, half a second): note on
    track.insert(track.end(), {0x83, 0x60, 0x90, 0x40, 0x64});
    // delta 960: note off
    track.insert(track.end(), {0x87, 0x40, 0x80, 0x40, 0x00});
    track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(out, 6);
    be16(out, 0);
    be16(out, 1);
    be16(out, 480);
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    be32(out, static_cast<std::uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

/// `tinyMidi()` with a Roland GS reset in front of it.
///
/// This is how a sequence says what it is. `F0 41 10 42 12 40 00 7F 00 41 F7`
/// is a Roland DT1 write of 0x7F to address 40 00 7F, which is GS Reset, and it
/// is what a GS file sends before it plays a note. The 0x41 before the F7 is
/// the Roland checksum -- 128 minus the low seven bits of the address and data
/// summed -- and a receiver that checks it drops the message when it is wrong.
inline std::vector<std::uint8_t> gsResetMidi() {
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
    track.insert(track.end(), {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
    // delta 0: the reset, as a meta-length-prefixed SysEx event.
    const std::vector<std::uint8_t> sysex{0x41, 0x10, 0x42, 0x12, 0x40, 0x00,
                                          0x7F, 0x00, 0x41, 0xF7};
    track.push_back(0x00);
    track.push_back(0xF0);
    track.push_back(static_cast<std::uint8_t>(sysex.size()));
    track.insert(track.end(), sysex.begin(), sysex.end());
    // Then an ordinary note, so the file has something to render.
    track.insert(track.end(), {0x00, 0x90, 0x3C, 0x64});
    track.insert(track.end(), {0x87, 0x40, 0x80, 0x3C, 0x00});
    track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(out, 6);
    be16(out, 0);
    be16(out, 1);
    be16(out, 480);
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    be32(out, static_cast<std::uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

/// `sequence` wrapped as RMID, carrying `bank` as its embedded soundbank.
///
/// RMID is a RIFF container around a standard MIDI file, and the interesting
/// case is that it may also carry the whole SoundFont the music was written
/// for. The layout below is not a free choice -- midi_processing's sniffer
/// requires the `data` chunk at offset 12 and reads it before it will look at a
/// nested `RIFF`, so a bank placed first is simply not seen.
///
/// The `LIST INFO` in between carries `DBNK`, which is how a file states its
/// bank offset rather than having one guessed by scanning the sequence for the
/// banks it selects. It also lets the parser stop the moment it has all three
/// chunks, which matters: the nested-RIFF branch advances by the chunk size
/// without its own 8-byte header, so anything after it is read from the wrong
/// place. Every file this is modelled on ends with the bank, so that has never
/// been reached in practice, and the fixture does not reach it either.
///
/// `bank` is a complete RIFF file -- an SF2 begins `RIFF....sfbk` -- because
/// what the container hands back is the nested chunk verbatim, header included.
inline std::vector<std::uint8_t> rmidWithBank(const std::vector<std::uint8_t>& sequence,
                                              const std::vector<std::uint8_t>& bank,
                                              std::uint16_t bankOffset) {
    const auto le32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int shift = 0; shift <= 24; shift += 8) {
            v.push_back(static_cast<std::uint8_t>((x >> shift) & 0xFF));
        }
    };

    std::vector<std::uint8_t> body;
    body.insert(body.end(), {'R', 'M', 'I', 'D'});

    body.insert(body.end(), {'d', 'a', 't', 'a'});
    le32(body, static_cast<std::uint32_t>(sequence.size()));
    body.insert(body.end(), sequence.begin(), sequence.end());
    if (sequence.size() % 2 == 1) {
        body.push_back(0);  // RIFF chunks are word-aligned
    }

    // LIST INFO holding one DBNK field, which is two little-endian bytes.
    std::vector<std::uint8_t> info;
    info.insert(info.end(), {'I', 'N', 'F', 'O'});
    info.insert(info.end(), {'D', 'B', 'N', 'K'});
    le32(info, 2);
    info.push_back(static_cast<std::uint8_t>(bankOffset & 0xFF));
    info.push_back(static_cast<std::uint8_t>(bankOffset >> 8));

    body.insert(body.end(), {'L', 'I', 'S', 'T'});
    le32(body, static_cast<std::uint32_t>(info.size()));
    body.insert(body.end(), info.begin(), info.end());

    body.insert(body.end(), bank.begin(), bank.end());

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    le32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// The smallest SoundFont that actually makes a sound: one preset, one
/// instrument, one zone, one sample.
///
/// Built rather than committed, and built rather than borrowed from
/// `XPCOG_SOUNDFONT`, because the bank these tests were written against is
/// 1.35 GB -- embedding that in an RMID would produce a 1.35 GB fixture, and
/// the decoder would refuse it long before the point being tested (there is a
/// 512 MB read limit on a source, and an RMID is meant to be a small
/// self-contained file). A few kilobytes here runs the same path on CI.
///
/// The structure is the SF2 specification's minimum and every part of it is
/// load-bearing. Each of the three record lists ends with a terminal entry the
/// spec requires -- `EOP`, `EOI`, `EOS` -- and a loader that finds none reads
/// one record past the end. The generator in a preset zone must be
/// `instrument` (41) and the one in an instrument zone must be `sampleID` (53),
/// each last in its zone. The sample data is followed by the 46 zero samples
/// the spec demands after every sample.
inline std::vector<std::uint8_t> tinySoundFont() {
    const auto le16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    const auto le32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int shift = 0; shift <= 24; shift += 8) {
            v.push_back(static_cast<std::uint8_t>((x >> shift) & 0xFF));
        }
    };
    const auto name20 = [](std::vector<std::uint8_t>& v, std::string_view text) {
        for (std::size_t i = 0; i < 20; ++i) {
            v.push_back(i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
        }
    };
    /// A chunk: four-character id, little-endian size, payload, pad to even.
    const auto chunk = [&le32](std::vector<std::uint8_t>&       out,
                               std::string_view                 id,
                               const std::vector<std::uint8_t>& payload) {
        out.insert(out.end(), id.begin(), id.end());
        le32(out, static_cast<std::uint32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        if (payload.size() % 2 == 1) {
            out.push_back(0);
        }
    };

    // --- INFO -----------------------------------------------------------
    std::vector<std::uint8_t> info;
    info.insert(info.end(), {'I', 'N', 'F', 'O'});
    {
        std::vector<std::uint8_t> ifil;
        le16(ifil, 2);  // SoundFont 2.01
        le16(ifil, 1);
        chunk(info, "ifil", ifil);

        // Zero-terminated, as every SF2 text field is -- and built by
        // appending the terminator rather than reading one past the string's
        // end, which is what `end() + 1` on a std::string iterator does.
        const auto zeroTerminated = [](std::string_view text) {
            std::vector<std::uint8_t> bytes(text.begin(), text.end());
            bytes.push_back(0);
            return bytes;
        };
        chunk(info, "isng", zeroTerminated("EMU8000"));
        chunk(info, "INAM", zeroTerminated("XPCog test bank"));
    }

    // --- sdta: one cycle of a sine, then the required silent tail --------
    constexpr std::uint32_t kSampleFrames = 1024;
    constexpr std::uint32_t kSilentTail   = 46;
    std::vector<std::uint8_t> sdta;
    sdta.insert(sdta.end(), {'s', 'd', 't', 'a'});
    {
        std::vector<std::uint8_t> smpl;
        for (std::uint32_t i = 0; i < kSampleFrames; ++i) {
            const double phase = xpcog::test::kTwoPi * static_cast<double>(i) /
                                 static_cast<double>(kSampleFrames);
            le16(smpl, static_cast<std::uint16_t>(
                           static_cast<std::int16_t>(20000.0 * std::sin(phase))));
        }
        for (std::uint32_t i = 0; i < kSilentTail; ++i) {
            le16(smpl, 0);
        }
        chunk(sdta, "smpl", smpl);
    }

    // --- pdta -----------------------------------------------------------
    std::vector<std::uint8_t> pdta;
    pdta.insert(pdta.end(), {'p', 'd', 't', 'a'});
    {
        std::vector<std::uint8_t> phdr;
        name20(phdr, "Test");
        le16(phdr, 0);   // preset 0
        le16(phdr, 0);   // bank 0
        le16(phdr, 0);   // first bag
        le32(phdr, 0);   // library
        le32(phdr, 0);   // genre
        le32(phdr, 0);   // morphology
        name20(phdr, "EOP");
        le16(phdr, 0);
        le16(phdr, 0);
        le16(phdr, 1);   // one past the last bag
        le32(phdr, 0);
        le32(phdr, 0);
        le32(phdr, 0);
        chunk(pdta, "phdr", phdr);

        std::vector<std::uint8_t> pbag;
        le16(pbag, 0);  // generators start at 0
        le16(pbag, 0);  // modulators start at 0
        le16(pbag, 1);  // terminal
        le16(pbag, 0);
        chunk(pdta, "pbag", pbag);

        // One terminal modulator record, all zero, in each of the two lists.
        const std::vector<std::uint8_t> terminalMod(10, 0);
        chunk(pdta, "pmod", terminalMod);

        std::vector<std::uint8_t> pgen;
        le16(pgen, 41);  // instrument
        le16(pgen, 0);   // index 0
        le16(pgen, 0);   // terminal
        le16(pgen, 0);
        chunk(pdta, "pgen", pgen);

        std::vector<std::uint8_t> inst;
        name20(inst, "Inst");
        le16(inst, 0);
        name20(inst, "EOI");
        le16(inst, 1);
        chunk(pdta, "inst", inst);

        std::vector<std::uint8_t> ibag;
        le16(ibag, 0);
        le16(ibag, 0);
        le16(ibag, 1);
        le16(ibag, 0);
        chunk(pdta, "ibag", ibag);

        chunk(pdta, "imod", terminalMod);

        std::vector<std::uint8_t> igen;
        le16(igen, 53);  // sampleID
        le16(igen, 0);
        le16(igen, 0);   // terminal
        le16(igen, 0);
        chunk(pdta, "igen", igen);

        std::vector<std::uint8_t> shdr;
        name20(shdr, "Sine");
        le32(shdr, 0);                 // start
        le32(shdr, kSampleFrames);     // end
        le32(shdr, 0);                 // loop start
        le32(shdr, kSampleFrames);     // loop end
        le32(shdr, 44100);             // sample rate
        shdr.push_back(60);            // original pitch: middle C
        shdr.push_back(0);             // pitch correction
        le16(shdr, 0);                 // sample link
        le16(shdr, 1);                 // monoSample
        name20(shdr, "EOS");
        le32(shdr, 0);
        le32(shdr, 0);
        le32(shdr, 0);
        le32(shdr, 0);
        le32(shdr, 0);
        shdr.push_back(0);
        shdr.push_back(0);
        le16(shdr, 0);
        le16(shdr, 0);
        chunk(pdta, "shdr", shdr);
    }

    std::vector<std::uint8_t> body;
    body.insert(body.end(), {'s', 'f', 'b', 'k'});
    chunk(body, "LIST", info);
    chunk(body, "LIST", sdta);
    chunk(body, "LIST", pdta);

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    le32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// A RIFF blob shaped like a SoundFont and holding nothing playable.
///
/// Enough for the container tests, which are about whether the bytes come back
/// whole and with the right offset. Whether they *load* is the synthesiser's
/// question and needs a real bank -- see test_midi_soundfont.cpp.
inline std::vector<std::uint8_t> fakeSoundBank(std::size_t payload = 64) {
    std::vector<std::uint8_t> body;
    body.insert(body.end(), {'s', 'f', 'b', 'k'});
    for (std::size_t i = 0; i < payload; ++i) {
        body.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }

    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    for (int shift = 0; shift <= 24; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((body.size() >> shift) & 0xFF));
    }
    out.insert(out.end(), body.begin(), body.end());
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
