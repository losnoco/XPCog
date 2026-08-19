#include "midi/MidiFile.hpp"

#include <midi_container.h>
#include <midi_processor.h>

#include <algorithm>
#include <string>

namespace xpcog::codecs {

struct MidiFile::Impl {
    midi_container container;
    bool           parsed = false;
};

MidiFile::MidiFile() : impl_(std::make_unique<Impl>()) {}
MidiFile::~MidiFile() = default;
MidiFile::MidiFile(MidiFile&&) noexcept            = default;
MidiFile& MidiFile::operator=(MidiFile&&) noexcept = default;

bool MidiFile::parse(const std::vector<std::uint8_t>& bytes,
                     std::string_view                 extension) {
    impl_          = std::make_unique<Impl>();
    impl_->parsed  = false;
    if (bytes.empty()) {
        return false;
    }

    // The extension is a hint, not a decision: midi_processor sniffs content
    // too. It is passed with no leading dot, which is how the library wants it
    // and is also how Url::extension() gives it.
    const std::string ext{extension};
    if (!midi_processor::process_file(bytes, ext.c_str(), impl_->container)) {
        return false;
    }

    // Loop points, which several of these formats state rather than imply --
    // XMI has its own, RPG Maker and Touhou files use markers, and a synth that
    // ignored them would play an intro once and stop where the composer meant
    // it to repeat. Scanned here because it is a property of the sequence, not
    // of whatever renders it.
    impl_->container.scan_for_loops(true, true, true, true);

    impl_->parsed = impl_->container.get_subsong_count() > 0;
    return impl_->parsed;
}

bool MidiFile::valid() const noexcept { return impl_ && impl_->parsed; }

std::size_t MidiFile::subsongCount() const {
    return valid() ? static_cast<std::size_t>(impl_->container.get_subsong_count()) : 0;
}

double MidiFile::duration(std::size_t subsong) const {
    if (!valid() || subsong >= subsongCount()) {
        return 0.0;
    }
    const auto index = impl_->container.get_subsong(static_cast<unsigned long>(subsong));
    return impl_->container.get_timestamp_end(index, /*seconds=*/true);
}

MidiLoop MidiFile::loop(std::size_t subsong) const {
    MidiLoop result;
    if (!valid() || subsong >= subsongCount()) {
        return result;
    }
    const auto index = impl_->container.get_subsong(static_cast<unsigned long>(subsong));

    // The library reports "no loop point" by returning ~0UL, cast to double --
    // so the test is against that value and not against zero, which is a
    // perfectly ordinary loop start.
    constexpr double kUnset = static_cast<double>(~0UL);

    const double start = impl_->container.get_timestamp_loop_start(index, /*seconds=*/true);
    const double end   = impl_->container.get_timestamp_loop_end(index, /*seconds=*/true);
    if (start == kUnset && end == kUnset) {
        return result;
    }

    // Either endpoint alone is a loop. A stated end with no start means "repeat
    // from the beginning"; a stated start with no end means "repeat to the end".
    result.valid = true;
    result.start = (start == kUnset) ? 0.0 : start;
    result.end   = (end == kUnset) ? duration(subsong) : end;
    return result;
}

MetadataMap MidiFile::metadata(std::size_t subsong) const {
    MetadataMap tags;
    if (!valid() || subsong >= subsongCount()) {
        return tags;
    }

    const auto     index = impl_->container.get_subsong(static_cast<unsigned long>(subsong));
    midi_meta_data meta;
    impl_->container.get_meta_data(index, meta);

    // The library's own names, mapped onto ours where they correspond and kept
    // verbatim where they do not. A MIDI file has no tag block: what is here is
    // whatever the sequence happened to state in its text meta events, so most
    // files produce a track name and nothing else.
    for (std::size_t i = 0; i < meta.get_count(); ++i) {
        const midi_meta_data_item& item = meta[i];
        if (item.m_name.empty() || item.m_value.empty()) {
            continue;
        }
        std::string key = item.m_name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // A track name becomes the title, and which track carries it depends on
        // the subsong: get_meta_data names these `track_name_NN` by *track*
        // index, and for a format-2 file it emits only the one track that is
        // this subsong -- so song 1's name is `track_name_01` and nothing is
        // called `track_name_00` at all. Matching the prefix rather than the
        // first index is what makes a subsong past the first have a name.
        //
        // A format-1 file can name several of its tracks. The first stands as
        // the title and the rest are kept under their own keys, which is the
        // same choice Cog makes.
        if (key == "title" || key.rfind("track_name_", 0) == 0) {
            if (tags.first("title").empty()) {
                tags.set("title", item.m_value);
                continue;
            }
        }
        tags.add(key, item.m_value);
    }
    return tags;
}

MidiStream MidiFile::stream(std::size_t subsong, double sampleRate) const {
    MidiStream out;
    if (!valid() || subsong >= subsongCount() || sampleRate <= 0.0) {
        return out;
    }

    const auto index = impl_->container.get_subsong(static_cast<unsigned long>(subsong));

    std::vector<midi_stream_event> events;
    system_exclusive_table         sysex;
    unsigned long                  loopStart = 0;
    unsigned long                  loopEnd   = 0;
    impl_->container.serialize_as_stream(index, events, sysex, loopStart, loopEnd, 0);

    out.events.reserve(events.size());
    for (const midi_stream_event& event : events) {
        MidiStreamEvent converted;
        // m_timestamp is seconds, as a double -- serialize_as_stream builds it
        // from the tempo map rather than from ticks, so the conversion to
        // samples belongs here and not in whatever renders it.
        converted.timestampSamples =
            static_cast<std::uint64_t>(event.m_timestamp * sampleRate);
        // Bit 31 marks a SysEx, and the rest is then an index into `sysex`
        // above -- which is why that table will have to travel with the stream
        // once a synth wants one. Nuked OPL3 does not: the chip has no SysEx to
        // receive, so the first synth behind this drops them.
        converted.isSysex = (event.m_event & 0x80000000U) != 0;
        if (converted.isSysex) {
            converted.message = static_cast<std::uint32_t>(event.m_event & 0x7FFFFFFFU);
        } else {
            converted.message = static_cast<std::uint32_t>(event.m_event & 0x00FFFFFFU);
            converted.port =
                static_cast<std::uint8_t>((event.m_event >> 24) & 0x7FU);
        }
        out.events.push_back(converted);
    }

    // The library signals "no loop" with ~0UL here too, and otherwise gives an
    // index into the stream it just built.
    if (loopStart != ~0UL && loopStart < out.events.size()) {
        out.loopStart = static_cast<std::size_t>(loopStart);
    }
    if (loopEnd != ~0UL && loopEnd <= out.events.size()) {
        out.loopEnd = static_cast<std::size_t>(loopEnd);
    }
    return out;
}

}  // namespace xpcog::codecs
