#include "common/TextEncoding.hpp"

#include "midi/MidiFile.hpp"

#include <spessasynth/midi/midi.h>

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>

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
    return valid() ? std::size_t{impl_->container->format == 2 ? impl_->container->track_count : 1} : 0;
}

double MidiFile::duration(std::size_t subsong) const {
    (void)subsong;
    if(!valid() || subsong >= subsongCount()) return 0.0;
    const SS_MIDIFile&  midi  = *impl_->container;
    if(midi.format != 2) {
        return midi.duration;
    }
    const SS_MIDITrack& track = midi.tracks[subsong];
    const size_t firstTick    = track.events[0].ticks;
    const size_t lastTick     = track.events[track.event_count - 1].ticks;
    const double startTime    = ss_midi_ticks_to_seconds(&midi, firstTick);
    const double endTime      = ss_midi_ticks_to_seconds(&midi, lastTick);
    return endTime - startTime;
}

MidiLoop MidiFile::loop(std::size_t subsong) const {
    (void)subsong;
    MidiLoop result;
    if (!valid() || subsong >= subsongCount()) {
        return result;
    }

    size_t start, end;

    double timeOffset = 0.0;

    if(impl_->container->format != 2) {
        start = impl_->container->loop.start;
        end   = impl_->container->loop.end;
    } else {
        const SS_MIDITrack *t = &impl_->container->tracks[subsong];
        start      = t->loop.start;
        end        = t->loop.end;
        timeOffset = ss_midi_ticks_to_seconds(impl_->container, t->events[0].ticks);
    }

    if (start == end) {
        return result;
    }

    // Either endpoint alone is a loop. A stated end with no start means "repeat
    // from the beginning"; a stated start with no end means "repeat to the end".
    result.valid = true;
    result.start = ss_midi_ticks_to_seconds(impl_->container, start) - timeOffset;
    result.end   = ss_midi_ticks_to_seconds(impl_->container, end) - timeOffset;
    return result;
}

MetadataMap MidiFile::metadata(std::size_t subsong) const {
    (void)subsong;
    MetadataMap tags;
    if (!valid() || subsong >= subsongCount()) {
        return tags;
    }

    const SS_RMIDIInfo& info = impl_->container->rmidi_info;
    auto trimString = [](std::string str) -> std::string {
        const std::string_view whitespace = " \t\n\r\f\v";

        const auto start = str.find_first_not_of(whitespace);
        if (start == std::string::npos) return "";

        const auto end = str.find_last_not_of(whitespace);
        return str.substr(start, end - start + 1);
    };
    auto tagSet = [&tags, trimString](const char* name, uint8_t* value, size_t len) {
        if (value && len) {
            const char* valstring = reinterpret_cast<const char*>(value);
            std::string val(valstring, len);
            auto v = trimString(val);
            if (!v.empty())
                tags.set(name, codecs::toUtf8(v));
        }
    };
    if (impl_->container->format != 2) {
        tagSet("title", info.name, info.name_len);
    }
    tagSet("artist", info.artist, info.artist_len);
    tagSet("album", info.album, info.album_len);
    tagSet("genre", info.genre, info.genre_len);
    tagSet("comment", info.comment, info.comment_len);
    tagSet("copyright", info.copyright, info.copyright_len);
    tagSet("date", info.creation_date, info.creation_date_len);
    tagSet("engineer", info.engineer, info.engineer_len);
    tagSet("software", info.software, info.software_len);
    tagSet("subject", info.subject, info.subject_len);

    if (tags.first("title").empty()) {
        if (impl_->container->format != 2) {
            for (size_t ti = 0; ti < impl_->container->track_count; ti++) {
                const SS_MIDITrack* t = &impl_->container->tracks[ti];
                auto val = trimString(t->name);
                if (!val.empty()) {
                    tags.set("title", codecs::toUtf8(val));
                    break;
                }
            }
        } else {
            const SS_MIDITrack* t = &impl_->container->tracks[subsong];
            auto val = trimString(t->name);
            if (!val.empty()) {
                tags.set("title", codecs::toUtf8(val));
            }
        }
    }

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

    size_t startTick = 0;
    size_t endTick   = midi.last_voice_event_tick;

    double oneTickSec = (midi.time_division > 0) ? (60.0 / (120.0 *
        (double)midi.time_division)) : (60.0 / (120.0 * 480.0));

    if (midi.format == 2) {
        const SS_MIDITrack& t = midi.tracks[subsong];
        startTick = t.events[0].ticks;
        endTick   = t.events[t.event_count - 1].ticks;

        struct ChannelState {
            std::optional<std::uint8_t>          program;
            std::optional<std::uint32_t>         bend;
            std::map<std::uint8_t, std::uint8_t> controllers;
        };
        std::array<ChannelState, 64> channels;
        std::vector<MidiSysex> sysex;

        std::size_t trackIndex;
        for(trackIndex = 0; trackIndex < subsong; trackIndex++) {
            const SS_MIDITrack& track = midi.tracks[trackIndex];
            std::size_t index = 0;
            for (; index < track.event_count &&
                   track.events[index].ticks < startTick;
                   ++index) {
                const SS_MIDIMessage& event = track.events[index];
                if (event.status_byte == SS_META_SET_TEMPO) {
                    double bpm = read_tempo_bpm(event.data);
                    if (midi.time_division > 0)
                        oneTickSec = 60.0 / (bpm * (double)midi.time_division);
                    continue;
                }
                if (event.status_byte == 0xF0) {
                    std::vector<std::uint8_t> data;
                    data.push_back(event.status_byte);
                    data.insert(data.end(), event.data, event.data + event.data_length);
                    sysex.push_back({data, static_cast<std::uint8_t>(effective_port(midi, event))});
                    continue;
                }
                const auto port = effective_port(midi, event);
                if (port >= 4) continue;
                const auto status  = event.status_byte;
                const auto channel = static_cast<std::size_t>((status & 0x0F) + port * 16);
                switch (status & 0xF0) {
                    case 0xB0:
                        if (event.data_length < 2) continue;
                        channels[channel].controllers[event.data[0x00] & 0x7F] =
                            static_cast<std::uint8_t>(event.data[0x01] & 0x7F);
                        break;
                    case 0xC0:
                        if (event.data_length < 1) continue;
                        channels[channel].program =
                            static_cast<std::uint8_t>(event.data[0x00] & 0x7F);
                        break;
                    case 0xE0:
                        if (event.data_length < 2) continue;
                        channels[channel].bend = status | ((event.data[0x00] & 0x7F) << 8) |
                            ((event.data[0x01] & 0x7F) << 16);
                        break;
                    default:
                        // Notes and aftertouch. Nothing here should still be
                        // sounding at the seek point.
                        break;
                }
            }
        }

        size_t eventCount = sysex.size();
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const ChannelState& state  = channels[channel];
            eventCount += state.controllers.size();
            if (state.program) {
                eventCount++;
            }
            if (state.bend) {
                eventCount++;
            }
        }

        out.events.reserve(eventCount + t.event_count);

        for (const auto& entry : sysex) {
            std::uint32_t index = static_cast<std::uint32_t>(out.sysex.size());
            out.events.push_back({0, index, entry.port, true});
            out.sysex.push_back(entry);
        }
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const ChannelState& state  = channels[channel];
            const auto          status = static_cast<std::uint32_t>(channel & 0x0F);
            const auto          port   = static_cast<std::uint8_t>(channel / 16);
            for (const auto& [controller, value] : state.controllers) {
                const uint32_t message = 0xB0u | status | (static_cast<std::uint32_t>(controller) << 8) |
                    (std::uint32_t{value} << 16);
                out.events.push_back({0, message, port, false});
            }
            if (state.program) {
                const uint32_t message = 0xC0u | status | (static_cast<std::uint32_t>(*state.program) << 8);
                out.events.push_back({0, message, port, false});
            }
            if (state.bend) {
                out.events.push_back({0, *state.bend, port, false});
            }
        }
    } else {
        size_t eventCount = 0;
        for (size_t ti = 0; ti < midi.track_count; ti++) {
            eventCount += midi.tracks[ti].event_count;
        }
        out.events.reserve(eventCount);
    }

    size_t loopStart, loopEnd;

    if (midi.format != 2) {
        loopStart = midi.loop.start;
        loopEnd   = midi.loop.end;
    } else {
        loopStart = midi.tracks[subsong].loop.start;
        loopEnd   = midi.tracks[subsong].loop.end;
    }

    size_t loopStartEvent = ~0UL;
    size_t loopEndEvent   = ~0UL;

    std::vector<std::size_t> currentEvents(midi.track_count, 0);

    size_t lastTicks = startTick;
    double timestamp = 0.0;

    for (;;) {
        const SS_MIDIMessage *msg = NULL;
		size_t bestTicks = ~0UL;
		for (size_t ti = 0; ti < midi.track_count; ti++) {
			const SS_MIDITrack& t = midi.tracks[ti];
			const size_t currentEvent = currentEvents[ti];
			if (currentEvent >= t.event_count) continue;
			const SS_MIDIMessage *_msg = &t.events[currentEvent];
            if (_msg->ticks < startTick) continue;
            if (_msg->ticks > endTick) continue;
			if (_msg->ticks < bestTicks) {
				bestTicks = _msg->ticks;
				msg = _msg;
			}
		}
		if(!msg) break;
		currentEvents[msg->track_index]++;

        if (loopStartEvent == ~0UL && msg->ticks == loopStart) {
            loopStartEvent = out.events.size();
        }

        uint8_t sb = msg->status_byte;
        if (sb == SS_META_SET_TEMPO) {
            double bpm = read_tempo_bpm(msg->data);
            if (midi.time_division > 0)
                oneTickSec = 60.0 / (bpm * (double)midi.time_division);
            continue;
        }

        MidiStreamEvent converted;

        size_t deltaTicks = msg->ticks - lastTicks;
        lastTicks = msg->ticks;

        timestamp += static_cast<double>(deltaTicks) * oneTickSec;

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
            std::uint32_t ev = sb;
            for(size_t i = 0; i < msg->data_length && i < 3; i++) {
                ev |= msg->data[i] << (i + 1) * 8;
            }
            converted.isSysex = false;
            converted.message = ev;
            converted.port = static_cast<std::uint8_t>(effective_port(midi, *msg));
            out.events.push_back(converted);
        } else if (sb == 0xF0) {
            std::vector<std::uint8_t> data;
            converted.isSysex = true;
            converted.message = static_cast<std::uint32_t>(out.sysex.size());
            converted.port = effective_port(midi, *msg);
            data.reserve(msg->data_length + 1);
            data.push_back((std::uint8_t)sb);
            data.insert(data.end(), msg->data, msg->data + msg->data_length);
            out.sysex.push_back({data, converted.port});
            out.events.push_back(converted);
        }

        if (msg->ticks == loopEnd) {
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

    auto data = stream(subsong, 1.0);

    for (const auto& sysex : data.sysex) {

        // SpessaSynth matches against the payload with the leading 0xF0 already
        // stripped, because it keeps the status byte in its own field
        // (midi_loader.c:852). midi_processing stores the message whole, so
        // step over it -- and tolerate either, since which one is being held is
        // exactly the kind of thing that changes underneath a caller.
        std::span<const uint8_t> body{sysex.data};
        if (body.front() == 0xF0) {
            body = body.subspan(1);
        }

        // GM2 On: 7E 7F 09 03. Universal non-real-time, all devices.
        if (!out.gm2 && body.size() >= 4 && body[0] == 0x7E && body[1] == 0x7F &&
            body[2] == 0x09 && body[3] == 0x03) {
            out.gm2 = true;
        }

        // GS reset: a Roland DT1 (0x12) writing 7F to either the mode-set or
        // the GS-reset address. Ported from ss_midi_has_gs(), including its
        // not checking the manufacturer byte -- matching it exactly matters
        // more here than tightening it, since the point is to pick the same
        // bank Cog would.
        if (!out.gs && body.size() >= 8 && body[3] == 0x12 && body[2] == 0x42 &&
            body[5] == 0x00 && body[6] == 0x7F && body[7] == 0x00 &&
            (body[4] == 0x00 || body[4] == 0x40)) {
            out.gs = true;
        }

        if (out.gs && out.gm2) {
            break;
        }
    }

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
    bank.bytes.assign(data, data + size);
    bank.bankOffset = impl_->container->bank_offset;
    return bank;
}

}  // namespace xpcog::codecs
