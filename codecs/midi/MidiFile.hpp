// A parsed MIDI file: what every synth behind it needs, and nothing about
// sound.
//
// Port of the parsing half of Cog Plugins/MIDI/MIDIDecoder.mm and
// MIDIMetadataReader.mm. The split is deliberate and follows codecs/psf: the
// container lands on its own, and each synth lands behind it. Three of them are
// coming -- Nuked OPL3, SpessaSynth and Nuked SC-55 -- and every one reads
// exactly this and none of them reads it differently. See docs/MIDI.md.
//
// Nothing here registers a decoder. A `.mid` that opens and then produces
// silence is worse than one the player does not claim, which is the rule the
// PSF container set for the same reason.
//
// ---------------------------------------------------------------------------
// What a MIDI file is, for the purposes of the layer above
// ---------------------------------------------------------------------------
// Not audio, and not one song. It is a set of tracks of timestamped events with
// a tempo map, and a few of the formats midi_processing reads -- XMI above all,
// which is how id and Origin shipped their soundtracks -- hold *several*
// sequences in one file. So this exposes subsongs, and the decoder that follows
// will address them by URL fragment exactly as the PSF and SID containers do.

#pragma once

#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/Plugin.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::codecs {

/// One event, already flattened into playback order with its timestamp in
/// samples. This is what a synth consumes; producing it is the container's job.
struct MidiStreamEvent {
    /// Position in samples at the rate passed to stream(). The library states
    /// these in seconds; converting here is what the sample rate is for.
    std::uint64_t timestampSamples = 0;
    /// Packed status/data as midi_processing hands it over: a short message in
    /// the low three bytes, or -- when `isSysex` is set, which the library
    /// marks with bit 31 -- an index into its system-exclusive table.
    std::uint32_t message = 0;
    bool          isSysex = false;
};

/// The parsed file. Copyable only by moving the implementation; the event
/// vectors are large enough that anything else would be a mistake to allow.
class MidiFile {
public:
    MidiFile();
    ~MidiFile();
    MidiFile(MidiFile&&) noexcept;
    MidiFile& operator=(MidiFile&&) noexcept;
    MidiFile(const MidiFile&)            = delete;
    MidiFile& operator=(const MidiFile&) = delete;

    /// Parses `bytes`. `extension` steers which processor is tried first --
    /// midi_processing sniffs content as well, so a mislabelled file still
    /// parses, but a `.mus` and a `.mid` can begin alike and the hint settles
    /// it. Empty on failure, which is the only signal: the library reports by
    /// returning false and says nothing about why.
    [[nodiscard]] bool parse(const std::vector<std::uint8_t>& bytes,
                             std::string_view                 extension);

    [[nodiscard]] bool valid() const noexcept;

    /// How many sequences the file holds. One for almost everything; more for
    /// XMI, and the reason this is not simply assumed to be one.
    [[nodiscard]] std::size_t subsongCount() const;

    /// Playing time of `subsong`, in seconds. This is the end of the last event
    /// rather than a stated duration -- a MIDI file does not carry one.
    [[nodiscard]] double duration(std::size_t subsong) const;

    /// Track name, copyright, lyrics and the rest, as the file states them.
    [[nodiscard]] MetadataMap metadata(std::size_t subsong) const;

    /// The event stream for `subsong` at `sampleRate`, ready for a synth.
    /// Empty until a synth exists to want it; the call is here because the
    /// shape of it is what decides whether this layer is right.
    [[nodiscard]] std::vector<MidiStreamEvent> stream(std::size_t subsong,
                                                      double sampleRate) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Reads a whole source into memory. A MIDI file is small -- the largest in a
/// 197,000-file corpus is under 4 MB and the average is 19 KB -- and every
/// processor midi_processing has wants the whole thing at once, so there is no
/// streaming path to design around.
[[nodiscard]] std::vector<std::uint8_t> readAllBytes(ISource& source);

}  // namespace xpcog::codecs
