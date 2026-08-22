// Nuked OPL3, behind the two MIDI drivers Cog puts in front of it.
//
// Port of Cog Plugins/MIDI/MSPlayer.cpp, which is the file that takes a moment
// to recognise because nothing in its name says OPL. It is the wrapper around
// `midisynth`, the interface that vendor/nuked-opl3's two drivers implement.
//
// ---------------------------------------------------------------------------
// What this actually is
// ---------------------------------------------------------------------------
// A Yamaha YMF262 -- the OPL3 -- on a Sound Blaster 16, emulated at the level
// of its registers. It is not a sampler: it has eighteen two-operator FM voices
// and no idea what an instrument is, so every General MIDI program has to be
// approximated by a driver that knows which operator settings sound like a
// trumpet. Which driver is chosen therefore matters more than it would for a
// SoundFont synth, and the two here disagree on purpose:
//
//   Doom          id's driver from Chocolate Doom, with the six DMX instrument
//                 banks shipped with Doom, Doom II, Raptor and Strife. This is
//                 what a `.mus` sounded like in 1993, wrong instruments and
//                 all, and is the default for that reason.
//   GeneralMidi   Nuke.YKT's own driver with a full GM timbre set. Better on an
//                 arbitrary `.mid`; less faithful on a Doom one.
//
// ---------------------------------------------------------------------------
// What it cannot do
// ---------------------------------------------------------------------------
// SysEx. There is no register on the chip for it and neither driver has a path
// to receive one, so a GM/GS/XG reset -- the thing an SC-55 or a SoundFont
// synth needs before anything sounds right -- is simply dropped. Cog's MSPlayer
// drops it too. It costs nothing here because there is no bank map or reverb
// unit for such a message to configure.
//
// Nor more than one port. A file that names a second MIDI port wants a second
// synthesiser; there is one chip, and events beyond port 0 are dropped, which
// is again what Cog does.

#pragma once

#include "midi/MidiSynth.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

/// vendor/nuked-opl3/interface.h. Forward-declared so that header, which has no
/// include guard and declares its classes at global scope, stays inside the one
/// translation unit that needs it.
class midisynth;

namespace xpcog::codecs {

enum class OplDriver : std::uint8_t {
    /// DoomOPL: id's DMX driver and its instrument banks.
    Doom,
    /// OPL3W: Nuke.YKT's General MIDI driver.
    GeneralMidi,
};

class OplSynth final : public MidiSynth {
public:
    OplSynth() = default;
    ~OplSynth() override;

    /// Creates the driver and resets the chip. `bank` selects an instrument set
    /// and is clamped to what the driver offers. Returns false if the chip or
    /// its resampler could not be allocated, which is the only way this fails.
    [[nodiscard]] bool open(OplDriver driver, unsigned bank, double sampleRate);

    [[nodiscard]] bool isOpen() const noexcept { return synth_ != nullptr; }

    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }

    [[nodiscard]] const char* displayName() const noexcept override;

    void close();

    /// Delivers one short message, packed status/data0/data1 as
    /// MidiStreamEvent::message holds it. Anything the driver does not
    /// recognise is ignored by the driver itself.
    void write(std::uint32_t message) override;

    /// Renders `frames` stereo frames. The chip runs at a fixed 49716 Hz and a
    /// sinc resampler inside it produces the requested rate, so this is exact
    /// for any `frames` including zero.
    void render(float* out, std::size_t frames) override;

    /// Rebuilds the chip, which for an OPL3 is an allocation and a register
    /// clear -- there is nothing here that takes time to become ready.
    void reset() override;

    /// How many instrument banks `driver` offers: six for Doom, one for the
    /// General MIDI driver. Asked without opening anything, because the setting
    /// has to be validated before a file is.
    [[nodiscard]] static unsigned bankCount(OplDriver driver);

private:
    /// Where the chip's own 16-bit output lands on its way to being widened.
    /// A member rather than a local so a render does not allocate.
    std::vector<std::int16_t> pcm_;

    midisynth* synth_      = nullptr;
    OplDriver  driver_     = OplDriver::Doom;
    unsigned   bank_       = 0;
    double     sampleRate_ = 0.0;
};

}  // namespace xpcog::codecs
