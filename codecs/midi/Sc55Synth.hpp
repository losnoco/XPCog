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
//
// ---------------------------------------------------------------------------
// The LCD timestamp is a stream position, whatever api.h says
// ---------------------------------------------------------------------------
// api.h documents the callback's timestamp as "milliseconds, absolute elapsed
// since boot". Taken at face value that would make every captured frame land
// seven seconds late, since booting is seven seconds of emulated time before a
// note is ever sent.
//
// The code disagrees with the comment, and the code is what runs. The value is
// computed from `st->sample_counter` (mcu.cpp:982), which accumulates as the
// machine renders and is reset to zero *only* inside sc55_spin (mcu.cpp:1542) --
// which is the boot spin, and which zeroes it after every page. So by the time
// spinning is over the counter is back at nothing, and from the first real
// render onward it counts exactly the samples this synthesiser has produced.
//
// That is precisely the clock a display wants, and it is worth the paragraph
// because reading api.h instead of mcu.cpp gives an answer that is wrong by
// seven seconds and looks plausible. The test asserts the first captured frame
// lands near the start of the stream rather than seven seconds into it.

#pragma once

#include "midi/MidiSynth.hpp"
#include "midi/Sc55Roms.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>
#include <string>

struct sc55_state;

namespace xpcog::codecs {

/// One captured state of the front-panel LCD, positioned in the rendered stream.
///
/// The state is opaque -- it is the emulator's own `lcd_state_t`, and only
/// sc55_lcd_render_screen() knows how to turn it into pixels. What this layer
/// adds is *when*: the position, in samples from the start of playback, at
/// which the panel came to look like this.
struct Sc55LcdFrame {
    std::uint64_t          samplePosition = 0;
    std::vector<std::byte> state;
};

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

    /// Resets the machine over MIDI rather than rebooting it.
    ///
    /// A GS reset and an all-sound-off on every channel, then a quarter second
    /// of emulation so the firmware acts on them before anything else arrives.
    /// Rebooting instead would mean reloading the ROMs and spinning seven
    /// seconds of emulated time, which is the second a seek used to cost.
    void reset() override;

    /// Ties the emulator's sample counter to a position in the track.
    ///
    /// The counter measures what this machine has rendered, which is the same
    /// as the track position only while the two have advanced together. A seek
    /// breaks that -- the machine keeps counting from wherever it was while the
    /// track jumps -- so without this every panel state after a seek would be
    /// filed under the wrong moment and never drawn.
    void rebaseLcd(std::uint64_t trackSamples);

    /// Starts or stops capturing the front panel.
    ///
    /// Off by default, and worth a switch rather than always being on: capturing
    /// costs a comparison against the previous state on every emulated sample
    /// and a copy on every change, which is pure waste when no panel is on
    /// screen. Rendering goes through sc55_render_with_lcd only while this is
    /// set.
    void setCaptureLcd(bool capture) noexcept { captureLcd_ = capture; }

    [[nodiscard]] bool capturingLcd() const noexcept { return captureLcd_; }

    /// Takes everything captured since the last call, oldest first.
    ///
    /// Drained rather than read, because the consumer is a display that shows
    /// each state once and there is no reason to keep one after it has been
    /// shown. Nothing accumulates while nobody is draining: capture is only on
    /// when something is watching.
    [[nodiscard]] std::vector<Sc55LcdFrame> takeLcdFrames();

private:
    /// Called by the emulator whenever the panel changes; forwards to the
    /// instance, which is what `context` is.
    static void pushLcd(void* context, int port, const void* state, std::size_t size,
                        std::uint64_t timestampMs);
    void        onLcd(const void* state, std::size_t size, std::uint64_t timestampMs);

    sc55_state* state_      = nullptr;
    double      sampleRate_ = 0.0;
    /// The model the ROMs turned out to be, for displayName().
    std::string device_ = "Roland SC-55";

    bool                      captureLcd_ = false;
    /// Samples rendered since this machine was opened, and what has to be added
    /// to that to get a position in the track. See rebaseLcd().
    std::uint64_t rendered_ = 0;
    std::int64_t  lcdBias_  = 0;
    std::vector<Sc55LcdFrame> lcdFrames_;
    /// The timestamp of the last frame kept, so changes closer together than
    /// the throttle are collapsed.
    std::uint64_t lastLcdMs_  = 0;
    bool          haveLcdMs_  = false;
};

}  // namespace xpcog::codecs
