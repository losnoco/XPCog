#include "midi/Sc55Synth.hpp"

#include <api.h>

#include <algorithm>
#include <cstring>

namespace xpcog::codecs {
namespace {

/// How long the machine is run before it is asked to play anything.
///
/// Seven seconds of emulated time, which is Cog's number (SCPlayer.mm:210) and
/// is what the firmware takes to finish its own startup -- it clears memory,
/// puts "Roland SC-55mkII" on the panel and settles. Notes sent before that
/// lands are simply not heard, so this is not a margin that can be trimmed
/// without finding out empirically where the machine actually becomes ready.
constexpr double kBootSeconds = 7.0;

/// How many bytes a short message occupies on the wire.
///
/// The container packs status, data0 and data1 into one word and does not say
/// how many of them are real, so the status byte has to say -- which it does,
/// and has since 1983. Program change and channel pressure take one data byte;
/// everything else in the channel range takes two.
[[nodiscard]] std::size_t messageLength(std::uint8_t status) {
    if (status < 0x80) {
        return 0;  // not a status byte at all
    }
    switch (status & 0xF0) {
        case 0xC0:  // program change
        case 0xD0:  // channel pressure
            return 2;
        case 0xF0:
            switch (status) {
                case 0xF1:  // MIDI time code quarter frame
                case 0xF3:  // song select
                    return 2;
                case 0xF2:  // song position pointer
                    return 3;
                default:
                    // 0xF6 tune request and 0xF8..0xFF real time, all bare.
                    return 1;
            }
        default:
            return 3;
    }
}

/// Serves the ROM set to the emulator, in the shape api.h asks for.
///
/// Three cases, and Cog's loadRom answers them the same way: a null `size` is
/// an existence probe, which is how sc55_init works out which model it is
/// holding; a `size` too small to hold the file reports the size it needs and
/// fails; otherwise the length is reported and the bytes copied if there is
/// somewhere to put them.
int readRom(void* context, const char* name, std::uint8_t* buffer, std::uint32_t* size) {
    const auto* roms = static_cast<const Sc55RomSet*>(context);
    if (roms == nullptr || name == nullptr) {
        return -1;
    }
    const std::vector<std::byte>* data = roms->find(name);
    if (data == nullptr) {
        return -1;
    }
    if (size == nullptr) {
        return 0;
    }
    if (data->size() > *size) {
        *size = static_cast<std::uint32_t>(data->size());
        return -1;
    }
    *size = static_cast<std::uint32_t>(data->size());
    if (buffer != nullptr) {
        std::memcpy(buffer, data->data(), data->size());
    }
    return 0;
}

}  // namespace

Sc55Synth::~Sc55Synth() { close(); }

void Sc55Synth::close() {
    if (state_ != nullptr) {
        sc55_free(state_);
        state_ = nullptr;
    }
    sampleRate_ = 0.0;
}

bool Sc55Synth::open(const Sc55RomSet& roms) {
    close();

    // GS_RESET, as Cog does: the SC-55mkII powers up in GS mode and this is the
    // state a file expects to find it in.
    state_ = sc55_init(/*port=*/0, GS_RESET, &readRom,
                       const_cast<Sc55RomSet*>(&roms));
    if (state_ == nullptr) {
        return false;
    }

    const std::uint32_t rate = sc55_get_sample_rate(state_);
    if (rate == 0) {
        close();
        return false;
    }
    sampleRate_ = static_cast<double>(rate);

    // Which model the set turned out to be, for the track properties. The
    // emulator autodetects it from the filenames it was served, so this is what
    // the ROMs said rather than what the setting asked for.
    device_ = "Roland " + roms.device;

    sc55_spin(state_, static_cast<std::uint32_t>(sampleRate_ * kBootSeconds));
    return true;
}

void Sc55Synth::write(std::uint32_t message) {
    if (state_ == nullptr) {
        return;
    }
    const std::uint8_t bytes[3] = {
        static_cast<std::uint8_t>(message & 0xFF),
        static_cast<std::uint8_t>((message >> 8) & 0xFF),
        static_cast<std::uint8_t>((message >> 16) & 0xFF),
    };
    const std::size_t length = messageLength(bytes[0]);
    if (length == 0) {
        return;
    }
    sc55_write_uart(state_, bytes, static_cast<std::uint32_t>(length));
}

void Sc55Synth::writeSysex(std::span<const std::uint8_t> bytes) {
    if (state_ == nullptr || bytes.empty()) {
        return;
    }
    sc55_write_uart(state_, bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

void Sc55Synth::render(std::int16_t* out, std::size_t frames) {
    if (frames == 0) {
        return;
    }
    if (state_ == nullptr) {
        std::fill_n(out, frames * 2, std::int16_t{0});
        return;
    }
    // Chunked so a long render cannot overflow the uint32_t count, and because
    // the emulator advances a whole machine per call -- there is no advantage
    // to asking it for a million frames at once.
    constexpr std::size_t kMaxChunk = 4096;
    while (frames > 0) {
        const std::size_t todo = std::min(frames, kMaxChunk);
        sc55_render(state_, out, static_cast<std::uint32_t>(todo));
        out += todo * 2;
        frames -= todo;
    }
}

}  // namespace xpcog::codecs
