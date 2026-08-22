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
#include <optional>
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

/// One system-exclusive message, whole.
///
/// Kept out of the event list because these are variable length and most files
/// have none, while a busy sequence has tens of thousands of short messages.
/// A synth that cannot receive SysEx ignores this entirely -- Nuked OPL3 has no
/// register for one -- and a synth that can needs every byte: a GS or XG reset
/// is how an SC-55 is told which of its instrument maps to use, and without it
/// a file plays on whatever the machine happened to boot into.
struct MidiSysex {
    std::vector<std::uint8_t> data;
    std::uint8_t              port = 0;
};

/// A subsong's events, and where it repeats.
struct MidiStream {
    /// Absent loop point. Both fields hold this when the sequence states no
    /// loop, which is the common case -- a MIDI file usually just ends.
    static constexpr std::size_t kNoLoop = static_cast<std::size_t>(-1);

    std::vector<MidiStreamEvent> events;
    /// The payloads a `isSysex` event's `message` indexes. Sparse: an entry the
    /// stream never refers to is left empty rather than renumbered, so the
    /// indices stay the ones the library issued.
    std::vector<MidiSysex> sysex;
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

/// Which dialect a sequence announces about itself, by the reset it sends.
///
/// Not inferred from tags or a file name: a sequence that means GS says so with
/// a Roland reset before it plays a note, and one that means General MIDI Level
/// 2 sends the universal GM2 On message. Cog reads exactly this to choose
/// between the plain bank and the map that remaps XG onto GS
/// (MIDIDecoder.mm:263), and that is the only decision it drives.
struct MidiDialect {
    bool gs  = false;
    bool gm2 = false;

    [[nodiscard]] bool any() const noexcept { return gs || gm2; }
};

/// A bank an RMID brought inside itself.
///
/// RMID is a RIFF wrapper around a standard MIDI file, and one of the chunks it
/// may carry is a whole SoundFont or DLS bank -- so the file arrives with the
/// instruments its author wrote it for, rather than hoping the listener has
/// them. `bytes` is that chunk verbatim, headers and all, which is exactly what
/// a bank loader expects to be handed.
///
/// `bankOffset` is the number added to every bank-select the file makes, and it
/// is not decoration: a bank written to sit at MSB 8 plays the wrong
/// instruments at MSB 0. It comes from the RIFF `DBNK` field when the file
/// states one, and otherwise from midi_processing scanning the sequence for the
/// bank numbers it actually selects.
struct MidiEmbeddedBank {
    std::vector<std::uint8_t> bytes;
    std::uint16_t             bankOffset = 0;
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

    /// What `subsong` announces itself as. Both flags false is the ordinary
    /// case: most files announce nothing and are plain General MIDI.
    ///
    /// Per subsong rather than per file, which is where this differs from Cog:
    /// Cog scans every track of the whole file. The two answers can only differ
    /// for a format that holds several sequences at once, which in practice
    /// means XMI, and there the subsong being played is the better question.
    [[nodiscard]] MidiDialect dialect(std::size_t subsong) const;

    /// The bank this file carried, or nullopt when it carried none -- which is
    /// every file that is not an RMID, and most RMIDs.
    ///
    /// A copy rather than a view, because the parse buffer the caller handed to
    /// parse() is theirs to destroy and this outlives it. The banks that turn
    /// up here are small by SoundFont standards: an RMID is meant to be one
    /// self-contained file, so nobody embeds a gigabyte in one.
    [[nodiscard]] std::optional<MidiEmbeddedBank> embeddedBank() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::codecs
