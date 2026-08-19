// A parsed MIDI file: what every synth behind it needs, and nothing about
// sound.
//
// Port of the parsing half of Cog Plugins/MIDI/MIDIDecoder.mm and
// MIDIMetadataReader.mm. The split is deliberate and follows codecs/psf: the
// container lands on its own, and each synth lands behind it. Three of them are
// coming -- Nuked OPL3, SpessaSynth and Nuked SC-55 -- and every one reads
// exactly this and none of them reads it differently. See docs/MIDI.md.
//
// Nothing here registers a decoder -- MidiDecoder.cpp does that, and only
// because a synth now exists to answer it. Nuked OPL3 is the first; SpessaSynth
// and Nuked SC-55 follow, and both read exactly what is below.
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
    /// A short message -- status byte, then up to two data bytes -- packed into
    /// the low three bytes, or, when `isSysex` is set, an index into the file's
    /// system-exclusive table.
    ///
    /// midi_processing packs the port into bits 24-30 of the same word and
    /// marks a SysEx with bit 31; both are split out below rather than left
    /// here, because a synth handed the raw word would read a port-1 note-on as
    /// a message it has never seen.
    std::uint32_t message = 0;
    /// Which of the file's MIDI ports this belongs to. Almost always 0: a file
    /// only names a second port when it wants more than sixteen channels, which
    /// takes more than one synthesiser to play.
    std::uint8_t port = 0;
    bool         isSysex = false;
};

/// A subsong's events, and where it repeats.
struct MidiStream {
    /// Absent loop point. Both fields hold this when the sequence states no
    /// loop, which is the common case -- a MIDI file usually just ends.
    static constexpr std::size_t kNoLoop = static_cast<std::size_t>(-1);

    std::vector<MidiStreamEvent> events;
    /// Indices into `events`. These come from the library rather than being
    /// recomputed from loop() below, so a player rewinding to `loopStart`
    /// resumes on exactly the event the tempo map says it should.
    std::size_t loopStart = kNoLoop;
    std::size_t loopEnd   = kNoLoop;
};

/// Where a sequence repeats, in seconds.
///
/// Several of these formats state this rather than implying it -- XMI carries
/// its own loop controllers, and RPG Maker and Touhou files use text markers --
/// and it is what decides how long a track is: a file that loops has no natural
/// end, so the player invents one from the loop and a fade.
struct MidiLoop {
    /// False when the sequence states no loop, in which case it simply ends.
    bool valid = false;
    /// Zero when only an end is stated, which is a loop back to the beginning.
    double start = 0.0;
    /// The end of the sequence when only a start is stated.
    double end = 0.0;
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

    /// Where `subsong` repeats, or an invalid MidiLoop when it states no loop.
    /// Read from the same scan the event stream's loop indices come from.
    [[nodiscard]] MidiLoop loop(std::size_t subsong) const;

    /// Track name, copyright, lyrics and the rest, as the file states them.
    [[nodiscard]] MetadataMap metadata(std::size_t subsong) const;

    /// The event stream for `subsong` at `sampleRate`, ready for a synth.
    [[nodiscard]] MidiStream stream(std::size_t subsong, double sampleRate) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::codecs
