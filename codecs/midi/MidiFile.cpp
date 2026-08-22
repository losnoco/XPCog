#include "midi/MidiFile.hpp"

#include <spessasynth/midi/midi.h>

#include <algorithm>
#include <span>
#include <string>

namespace xpcog::codecs {

struct MidiFile::Impl {
    SS_MIDIFile* container;
    Impl() : container(nullptr) { }
    ~Impl() { ss_midi_free(container); }
};

MidiFile::MidiFile() : impl_(std::make_unique<Impl>()) {}
MidiFile::~MidiFile() = default;
MidiFile::MidiFile(MidiFile&&) noexcept            = default;
MidiFile& MidiFile::operator=(MidiFile&&) noexcept = default;

bool MidiFile::parse(const std::vector<std::uint8_t>& bytes,
                     std::string_view                 extension) {
    impl_          = std::make_unique<Impl>();
    if (bytes.empty()) {
        return false;
    }

    SS_File* file = ss_file_open_from_memory(&bytes[0],
                                             bytes.size(),
                                             false);
    if (file == nullptr) {
        return false;
    }

    // The extension is a hint, not a decision: midi_processor sniffs content
    // too. It is passed with no leading dot, which is how the library wants it
    // and is also how Url::extension() gives it.
    const std::string ext{extension};
    SS_MIDIFile* midi = ss_midi_load(file, ext.c_str());
    ss_file_close(file);

    impl_->container = midi;

    if (midi) {
        ss_midi_remove_emidi_non_gm(midi);
        ss_midi_flush(midi);
        if (!ss_midi_ensure_timeline(midi)) {
            return false;
        }
    }

    return !!impl_->container;
}

bool MidiFile::valid() const noexcept { return impl_ && !!impl_->container; }

std::size_t MidiFile::subsongCount() const {
    return valid() ? 1 : 0;
}

double MidiFile::duration(std::size_t subsong) const {
    (void)subsong;
    return valid() ? impl_->container->duration : 0.0;
}

MidiLoop MidiFile::loop(std::size_t subsong) const {
    (void)subsong;
    MidiLoop result;
    if (!valid()) {
        return result;
    }

    size_t start = impl_->container->loop.start;
    size_t end = impl_->container->loop.end;
    if (start == end) {
        return result;
    }

    // Either endpoint alone is a loop. A stated end with no start means "repeat
    // from the beginning"; a stated start with no end means "repeat to the end".
    result.valid = true;
    result.start = ss_midi_ticks_to_seconds(impl_->container, start);
    result.end   = ss_midi_ticks_to_seconds(impl_->container, end);
    return result;
}

MetadataMap MidiFile::metadata(std::size_t subsong) const {
    (void)subsong;
    MetadataMap tags;
    if (!valid()) {
        return tags;
    }

    const SS_RMIDIInfo& info = impl_->container->rmidi_info;
    auto tagSet = [&tags](const char* name, uint8_t* value, size_t len) {
        if (value && len) {
            const char* valstring = reinterpret_cast<const char*>(value);
            std::span<const char> valrange(valstring, len);
            std::stringstream val;
            for (const auto& ch : valrange) {
                val << ch;
            }
            tags.set(name, val.str());
        }
    };
    tagSet("title", info.name, info.name_len);
    tagSet("artist", info.artist, info.artist_len);
    tagSet("album", info.album, info.album_len);
    tagSet("genre", info.genre, info.genre_len);
    tagSet("comment", info.comment, info.comment_len);
    tagSet("copyright", info.copyright, info.copyright_len);
    tagSet("date", info.creation_date, info.creation_date_len);
    tagSet("engineer", info.engineer, info.engineer_len);
    tagSet("software", info.software, info.software_len);
    tagSet("subject", info.subject, info.subject_len);

    return tags;
}

[[nodiscard]] static double read_tempo_bpm(const uint8_t *d) {
	uint32_t us = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
	if (us == 0) us = 500000;
	return 60000000.0 / (double)us;
}

[[nodiscard]] static int effective_port(const SS_MIDIFile& midi,
                                        const SS_MIDIMessage& e) {
	if (!midi.is_multi_port || !midi.port_channel_offset_map) return 0;
	size_t ti = e.track_index;
	if (ti >= midi.track_count) return 0;
	int port = midi.tracks[ti].port;
	if (port < 0) return 0;
	if ((size_t)port >= midi.port_channel_offset_map_count) return 0;
	return midi.port_channel_offset_map[port] / 16;
}

MidiStream MidiFile::stream(std::size_t subsong, double sampleRate) const {
    MidiStream out;
    if (!valid() || subsong >= subsongCount() || sampleRate <= 0.0) {
        return out;
    }

    const SS_MIDIFile& midi = *impl_->container;

    size_t loopStart = midi.loop.start;
    size_t loopEnd   = midi.loop.end;

    size_t loopStartEvent = ~0UL;
    size_t loopEndEvent   = ~0UL;

    out.events.reserve(midi.timeline_count);
    std::span<SS_MIDIMessage> in(midi.timeline, midi.timeline_count);
    size_t lastTicks = 0;
    double timestamp = 0.0;
   	double one_tick_sec = (midi.time_division > 0) ? (60.0 / (120.0 *
        (double)midi.time_division)) : (60.0 / (120.0 * 480.0));
    for (const SS_MIDIMessage& event : in) {
        MidiStreamEvent converted;
        size_t deltaTicks = event.ticks - lastTicks;
        lastTicks = event.ticks;

        timestamp += static_cast<double>(deltaTicks) * one_tick_sec;

        uint8_t sb = event.status_byte;
        if (sb == SS_META_SET_TEMPO && event.data_length >= 3) {
            double bpm = read_tempo_bpm(event.data);
            if (midi.time_division > 0)
                one_tick_sec = 60.0 / (bpm * (double)midi.time_division);
            continue;
        }

        if (loopStartEvent == ~0UL &&
                event.ticks == loopStart) {
            loopStartEvent = out.events.size();
        }

        // m_timestamp is seconds, as a double -- serialize_as_stream builds it
        // from the tempo map rather than from ticks, so the conversion to
        // samples belongs here and not in whatever renders it.
        converted.timestampSamples =
            static_cast<std::uint64_t>(timestamp * sampleRate);
        // Bit 31 marks a SysEx, and the rest is then an index into the table
        // the library filled in beside the stream; the payloads are copied out
        // below. Nuked OPL3 ignores them -- the chip has no register to receive
        // one -- but an SC-55 needs them, since a GS reset is a SysEx.
        if (sb >= 0x80 && sb < 0xF0) {
            converted.isSysex = false;
            std::uint32_t ev = sb;
            for(size_t i = 0; i < event.data_length && i < 3; i++) {
                ev |= event.data[i] << (i + 1) * 8;
            }
            converted.message = ev;
        } else if (sb == 0xF0) {
            converted.isSysex = true;
            converted.sysex.reserve(event.data_length + 1);
            converted.sysex.push_back((std::uint8_t)sb);
            converted.sysex.insert(converted.sysex.end(), event.data,
                event.data + event.data_length);
        } else continue;

        converted.port = effective_port(midi, event);
        out.events.push_back(converted);

        if (event.ticks == loopEnd) {
            loopEndEvent = out.events.size();
        }
    }

    // The library signals "no loop" with ~0UL here too, and otherwise gives an
    // index into the stream it just built.
    if (loopStartEvent != ~0UL && loopStartEvent < out.events.size()) {
        out.loopStart = static_cast<std::size_t>(loopStartEvent);
    }
    if (loopEndEvent != ~0UL && loopEndEvent <= out.events.size()) {
        out.loopEnd = static_cast<std::size_t>(loopEndEvent);
    }
    return out;
}

MidiDialect MidiFile::dialect(std::size_t subsong) const {
    MidiDialect out;
    if (!valid() || subsong >= subsongCount()) {
        return out;
    }

    out.gs = ss_midi_has_gs(impl_->container);
    out.gm2 = ss_midi_has_gm2(impl_->container);

    return out;
}

std::optional<MidiEmbeddedBank> MidiFile::embeddedBank() const {
    if (!valid()) {
        return std::nullopt;
    }

    const std::uint8_t* data = impl_->container->embedded_soundbank;
    size_t size = impl_->container->embedded_soundbank_size;

    if (!data || !size) {
        return std::nullopt;
    }

    MidiEmbeddedBank bank;
    bank.bytes = std::span<const uint8_t>(data, size);
    bank.bankOffset = impl_->container->bank_offset;
    return bank;
}

}  // namespace xpcog::codecs
