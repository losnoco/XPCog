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
        if (key == "track_name_00" || key == "title") {
            tags.set("title", item.m_value);
        } else {
            tags.add(key, item.m_value);
        }
    }
    return tags;
}

std::vector<MidiStreamEvent> MidiFile::stream(std::size_t subsong,
                                              double      sampleRate) const {
    std::vector<MidiStreamEvent> out;
    if (!valid() || subsong >= subsongCount() || sampleRate <= 0.0) {
        return out;
    }

    const auto index = impl_->container.get_subsong(static_cast<unsigned long>(subsong));

    std::vector<midi_stream_event> events;
    system_exclusive_table         sysex;
    unsigned long                  loopStart = 0;
    unsigned long                  loopEnd   = 0;
    impl_->container.serialize_as_stream(index, events, sysex, loopStart, loopEnd, 0);

    out.reserve(events.size());
    for (const midi_stream_event& event : events) {
        MidiStreamEvent converted;
        // m_timestamp is seconds, as a double -- serialize_as_stream builds it
        // from the tempo map rather than from ticks, so the conversion to
        // samples belongs here and not in whatever renders it.
        converted.timestampSamples =
            static_cast<std::uint64_t>(event.m_timestamp * sampleRate);
        converted.message = static_cast<std::uint32_t>(event.m_event & 0x7FFFFFFFU);
        // Bit 31 marks a SysEx, and the rest is then an index into the table
        // above -- which is why that table will have to travel with the stream
        // once a synth wants one.
        converted.isSysex = (event.m_event & 0x80000000U) != 0;
        out.push_back(converted);
    }
    return out;
}

std::vector<std::uint8_t> readAllBytes(ISource& source) {
    std::vector<std::uint8_t> bytes;
    std::uint8_t              buffer[16384];
    for (;;) {
        const std::int64_t got = source.read(buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    return bytes;
}

}  // namespace xpcog::codecs
