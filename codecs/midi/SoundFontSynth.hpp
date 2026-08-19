// SpessaSynth: a MIDI file played on samples of the real instruments.
//
// Port of Cog Plugins/MIDI/SpessaPlayer.mm, and stage 2 of docs/MIDI.md. The
// engine is kode54's C port of SpessaSynth, an overlay port rather than a
// vendored tree (ports/spessasynth-core), so what is here is the wrapper and
// nothing else.
//
// The third synthesiser and the odd one out, in two ways worth stating before
// the API makes them look arbitrary.
//
// **It cannot play anything on its own.** The OPL3 synthesises from a driver's
// instrument definitions and the SC-55 plays Roland's own ROMs; this one is an
// engine with no sound in it until it is given a bank. That is why open() takes
// a path and why a missing bank is not an error the layer above can paper over
// -- there is nothing to fall back to inside this class.
//
// **It renders in blocks of 128 and is best driven that way.** The engine ramps
// gain, pan and filter across a block and steps its LFOs per block, so a caller
// asking for four samples at a time would run those ramps to completion in four
// samples. So this buffers: it renders whole blocks and hands out slices of
// them, which is what Cog does too (SpessaPlayer::getChunkSize is 128). The
// price is that an event can be applied up to a block early, since the audio
// for its own sample may already have been rendered -- 2.9 ms at 44100, and the
// same bound Cog lives with.

#pragma once

#include "midi/MidiSynth.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xpcog::codecs {

/// How a sample is read between its own points.
///
/// XPCog's `resampling` setting names quality tiers rather than algorithms (see
/// settings.def), so this is what those tiers mean to a wavetable synthesiser.
/// Cog maps its own algorithm names onto the same four.
enum class SoundFontInterpolation : std::uint8_t {
    Nearest,
    Linear,
    Hermite,
    Sinc,
};

/// The bank a MIDI file brings with it, if it brought one.
///
/// Cog's rule, kept whole: a file may be accompanied by its own SoundFont, and
/// three names are tried -- `song.mid.sf2`, `song.sf2`, and `Album/Album.sf2`
/// for a whole folder that shares one bank. Each is tried with every extension
/// the engine reads. This is how a game rip that ships its instruments plays
/// with them rather than with whatever the listener happens to have configured.
[[nodiscard]] std::optional<std::filesystem::path> findCompanionBank(
    const std::filesystem::path& midiFile);

class SoundFontSynth final : public MidiSynth {
public:
    SoundFontSynth();
    ~SoundFontSynth() override;

    /// Loads `bank` and prepares to render at `sampleRate`.
    ///
    /// `bank` is a SoundFont (`.sf2`, `.sf3`, `.sf2pack`), a DLS bank, or an
    /// `.sflist`/`.json` naming several with the ranges they cover. Returns
    /// false if it cannot be read -- there is no silent failure here, because a
    /// synthesiser with no bank renders silence and that is indistinguishable
    /// from a file that has nothing to say.
    ///
    /// A 1.3 GB bank is a normal thing to be handed and is not read into
    /// memory: the engine keeps the file open and decodes samples as the music
    /// reaches them.
    [[nodiscard]] bool open(const std::filesystem::path& bank, double sampleRate,
                            SoundFontInterpolation interpolation);

    [[nodiscard]] double sampleRate() const noexcept override { return sampleRate_; }

    void write(std::uint32_t message) override;
    void writeSysex(std::span<const std::uint8_t> bytes) override;
    void render(float* out, std::size_t frames) override;

    /// A GM reset, which the engine applies at once. Nothing is reloaded: the
    /// bank is what took the time to open and it has not changed.
    void reset() override;

    [[nodiscard]] const char* displayName() const noexcept override {
        return displayName_.c_str();
    }

private:
    /// SS_Processor and the bank handles are anonymous typedefs in the engine's
    /// headers and cannot be forward-declared, so they live behind this.
    struct Impl;

    /// Submits one message with the timestamp of the sample the caller is at.
    /// Anything stamped ahead of the engine's own clock is queued by the engine
    /// and applied when its rendering reaches it.
    void submit(const std::uint8_t* data, std::size_t length);

    std::unique_ptr<Impl> impl_;

    double      sampleRate_ = 0.0;
    std::string displayName_ = "SpessaSynth";

    /// One block of the engine's own rendering, and how much of it the caller
    /// has taken. See the note at the top of this file.
    std::vector<float> block_;
    std::size_t        blockFill_ = 0;
    std::size_t        blockTaken_ = 0;

    /// Frames handed to the caller, which is the clock events are stamped with.
    std::uint64_t callerFrames_ = 0;
};

}  // namespace xpcog::codecs
