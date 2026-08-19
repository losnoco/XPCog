// What the decoder needs of a synthesiser, and nothing about which one it is.
//
// Deliberately not introduced at stage 1, when Nuked OPL3 was the only
// implementation and an interface would have been a guess about the second. The
// second has arrived and disagrees with the first on almost everything -- one
// takes a packed word and cannot receive SysEx, the other takes bytes on a
// serial port and needs it; one renders at whatever rate it is asked for, the
// other only at its hardware's -- so what they still have in common is worth
// writing down. SpessaSynth is the third and adds nothing new to this list.
//
// Construction is not here. Each synthesiser is opened with the arguments it
// actually needs -- a driver and a bank, a ROM set, a SoundFont -- and only
// then is it one of these.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace xpcog::codecs {

class MidiSynth {
public:
    MidiSynth()          = default;
    virtual ~MidiSynth() = default;

    MidiSynth(const MidiSynth&)            = delete;
    MidiSynth& operator=(const MidiSynth&) = delete;

    /// The rate this renders at. For most synthesisers it is whatever they were
    /// opened with; for the SC-55 it is the hardware's and nothing else.
    [[nodiscard]] virtual double sampleRate() const noexcept = 0;

    /// One short message: status in the low byte, then up to two data bytes.
    virtual void write(std::uint32_t message) = 0;

    /// One system-exclusive message, 0xF0 to 0xF7 inclusive. Ignored by a
    /// synthesiser with nowhere to put it.
    virtual void writeSysex(std::span<const std::uint8_t> /*bytes*/) {}

    /// Renders `frames` stereo frames of 16-bit samples.
    virtual void render(std::int16_t* out, std::size_t frames) = 0;

    /// Returns to a known state as cheaply as the machine allows.
    ///
    /// For seeking, and "cheaply" is the whole point. Rebuilding an SC-55 means
    /// reloading its ROMs and running seven seconds of emulated time before it
    /// will answer -- about a second of real work, which is what made seeking
    /// feel broken. Telling a machine that is already running to reset itself
    /// costs a hundred bytes of MIDI and a quarter second of emulation.
    virtual void reset() = 0;

    /// A name for the thing that actually made the sound, for the track's
    /// properties. Which synthesiser ran is not always the one the setting
    /// asked for -- a missing ROM set falls back rather than refusing to play --
    /// so it is worth being able to see.
    [[nodiscard]] virtual const char* displayName() const noexcept = 0;
};

}  // namespace xpcog::codecs
