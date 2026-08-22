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

/// The bank XPCog ships, or nullopt when this build has none beside it.
///
/// This is what lets the SoundFont synthesiser be the default rather than the
/// OPL3: it is the one bank that needs nothing from the listener. Cog reaches
/// for its own the same way and for the same reason (MIDIDecoder.mm:260).
///
/// `wantsGsMap` picks between the two files shipped. The plain bank is an XG
/// bank; a sequence that announced itself as GS or GM2 is asking for
/// instruments at bank numbers the XG bank puts elsewhere, and the `.sflist`
/// beside it is a 246-entry remapping that puts them back. Cog makes exactly
/// this choice, on exactly this question.
[[nodiscard]] std::optional<std::filesystem::path> shippedBank(bool wantsGsMap);

class SoundFontSynth final : public MidiSynth {
public:
    SoundFontSynth();
    ~SoundFontSynth() override;

    /// Prepares `bank` for rendering.
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
    void addGlobalBank(const std::filesystem::path& bank);

    /// Prepares `bank` as a per-file bank.
    ///
    /// `bank` is the same as above, except it takes precedence over the above
    /// specified `bank` when mapping instruments.
    void addFileBank(const std::filesystem::path& bank);

    /// Prepares `bank` as a per-file embedded bank.
    ///
    /// `bank` takes even further precedence over the above two.
    ///
    /// The bytes are copied and kept. The engine's file primitive can be handed
    /// a buffer it does not own, and then "it is essential to keep that buffer
    /// somewhere for the lifetime of the file" -- the bank reads samples out of
    /// it as the music reaches them, exactly as the streaming path does with a
    /// file on disk. An embedded bank is small enough that owning a copy is the
    /// cheaper mistake to avoid.
    void addEmbeddedBank(std::span<const std::uint8_t> bank, int bankOffset);

    /// Opens the synthesizer for rendering.
    ///
    /// If none of the above loaded banks succeed, or a memory allocation error
    /// occurs, this will return an error.
    [[nodiscard]] bool open(double                 sampleRate,
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

    /// The half both open() and openEmbedded() share: a processor at
    /// `sampleRate`, with no bank in it yet. False when the rate is one the
    /// engine will not run at.
    [[nodiscard]] bool start(double sampleRate, SoundFontInterpolation interpolation);

    std::unique_ptr<Impl> impl_;

    /// Held for the global and per-file banks, if any
    std::optional<std::filesystem::path> globalBank_;
    std::optional<std::filesystem::path> fileBank_;

    /// Held only for openEmbedded(), whose bank reads out of it for as long as
    /// the synthesiser lives. Empty on the path that opens a file.
    std::vector<std::uint8_t> embeddedBank_;
    int bankOffset_ = 0;

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
