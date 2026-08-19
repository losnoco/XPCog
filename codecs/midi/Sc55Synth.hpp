// A Roland SC-55mkII, booted from its own firmware.
//
// Port of the playback half of Cog Plugins/MIDI/SCPlayer.mm. Cog drives up to
// four of these at once for a file that names four MIDI ports; this drives one,
// for the reason OplSynth gives -- a second port wants a second synthesiser, and
// nothing in this player has a use for four SC-55s yet.
//
// ---------------------------------------------------------------------------
// Three things that make it unlike the OPL
// ---------------------------------------------------------------------------
// **It is a computer, and it boots.** sc55_init loads the ROMs and then the
// machine has to run its own startup code before it will answer to anything --
// Cog spins it for seven seconds of emulated time, and so does this. That is
// real work, not a delay: opening a file on this synthesiser costs a fraction
// of a second of CPU before the first sample.
//
// **MIDI arrives as bytes on a serial port.** sc55_write_uart takes the wire
// format, so a short message is unpacked back into its one to three bytes and
// SysEx is handed over whole. That is the opposite of the OPL drivers, which
// take a packed word and have no SysEx path at all -- and it is why the
// container carries the payloads.
//
// **The sample rate is the hardware's.** There is no rate argument anywhere;
// sc55_get_sample_rate reports what the machine runs at, and the decoder takes
// that rather than `synthSampleRate`. A resampler downstream is the player's
// business, not the synthesiser's.

#pragma once

#include "midi/MidiSynth.hpp"
#include "midi/Sc55Roms.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

struct sc55_state;

namespace xpcog::codecs {

class Sc55Synth final : public MidiSynth {
public:
    Sc55Synth() = default;
    ~Sc55Synth() override;

    /// Loads `roms` into a machine and runs it until it has finished booting.
    /// The set is borrowed for the duration of the call only.
    [[nodiscard]] bool open(const Sc55RomSet& roms);

    [[nodiscard]] bool isOpen() const noexcept { return state_ != nullptr; }

    void close();

    /// What the emulated hardware runs at. Zero until open() has succeeded.
    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }

    [[nodiscard]] const char* displayName() const noexcept override { return device_.c_str(); }

    /// One short message, packed as MidiStreamEvent::message holds it. Unpacked
    /// here into the one, two or three bytes the machine expects on its
    /// serial port.
    void write(std::uint32_t message) override;

    /// One system-exclusive message, from its 0xF0 to its 0xF7.
    void writeSysex(std::span<const std::uint8_t> bytes) override;

    /// Renders `frames` stereo frames.
    void render(std::int16_t* out, std::size_t frames) override;

private:
    sc55_state* state_      = nullptr;
    double      sampleRate_ = 0.0;
    /// The model the ROMs turned out to be, for displayName().
    std::string device_ = "Roland SC-55";
};

}  // namespace xpcog::codecs
