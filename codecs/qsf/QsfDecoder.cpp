// Capcom CPS-2 arcade rips, through HighlyQuixotic. The eighth and last of
// HighlyComplete's cores, and the smallest.
//
// Port of the `type == 0x41` paths of Cog
// Plugins/HighlyComplete/HCDecoder.mm.
//
// Every other core in this tree boots something: a BIOS, a firmware, a
// cartridge header. This one boots nothing. A CPS-2 sound board is a Z80
// running a program out of ROM alongside the QSound DSP, and a QSF carries both
// ROMs outright -- so the file *is* the machine, and a miniqsf's entire
// contribution is usually one byte written over the Z80 ROM to pick a song.
//
// The section format is a fifth variant, and the simplest: a flat run of
// `[3-byte tag][LE32 offset][LE32 length][data]` chunks, where the tag names
// which of three ROMs the data lands in. `Z80` is the sound program, up to
// 256 KB; `SMP` is the PCM sample ROM, up to 8 MB; `KEY` is eleven bytes of
// Kabuki key.
//
// Kabuki is the reason for that third one. CPS-1.5 and CPS-2 sound boards
// shipped the Z80 as a custom part that decrypts opcodes on the fly, so the
// ROM as dumped is ciphertext and the core needs the game's key to run it.
// Rips of games whose ROMs were already decrypted simply omit the section, and
// a zero key is upstream's own signal to execute the ROM as it stands.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include <Core/qsound.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::uint8_t kQsfVersion = 0x41;  // qsf, miniqsf

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// The QSound DSP's own rate, and not a round number: the chip divides a
/// 60 MHz clock down to it. `qsound_clear_state` installs 24038 as the default
/// and Cog leaves it there, so nothing calls `qsound_set_rates`.
constexpr int kSampleRate = 24038;

/// Section header: three tag bytes, then offset and length as little-endian
/// 32-bit. Everything after is payload.
constexpr std::size_t kChunkHeaderSize = 11;

/// A Kabuki key is exactly eleven bytes and is refused at any other size, which
/// is upstream's check rather than a guess: two 32-bit swap keys, a 16-bit
/// address key and one XOR byte, all big-endian.
constexpr std::size_t kKeySize = 11;

constexpr std::size_t kMaxZ80Rom    = 0x40000;   // 256 KB
constexpr std::size_t kMaxSampleRom = 0x800000;  // 8 MB

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] std::uint16_t readBe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

/// qsound_init() builds tables shared by every emulator state, so it happens
/// once for the process rather than per track, as with sega_init() next door.
void initQsoundOnce() {
    static std::once_flag once;
    std::call_once(once, [] { qsound_init(); });
}

/// The three ROMs a QSF assembles, each grown on demand as sections arrive.
///
/// These outlive the emulator state on purpose. `qsound_set_z80_rom` and
/// `qsound_set_sample_rom` store the pointers rather than copying, so the
/// buffers have to stay put and stay at the same address for as long as the
/// core is running -- which is why they are members here and not locals in
/// start().
struct QsfRoms {
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> z80;
    std::vector<std::uint8_t> samples;
};

/// Places one section into the ROM it names.
///
/// Sections write at an absolute offset and may arrive out of order and
/// overlapping, so each ROM grows to fit and the gap is zero-filled. Later
/// writes win, which is how a miniqsf's single song-select byte lands on top of
/// the library's Z80 image.
[[nodiscard]] bool uploadSection(QsfRoms& roms, std::string_view tag,
                                 std::uint32_t offset, const std::uint8_t* data,
                                 std::size_t size) {
    std::vector<std::uint8_t>* rom = nullptr;
    std::size_t                limit = 0;

    if (tag == "KEY") {
        rom   = &roms.key;
        limit = kKeySize;
    } else if (tag == "Z80") {
        rom   = &roms.z80;
        limit = kMaxZ80Rom;
    } else if (tag == "SMP") {
        rom   = &roms.samples;
        limit = kMaxSampleRom;
    } else {
        // An unknown tag means the file is not what it claims, rather than
        // something to skip: the three are the whole format.
        return false;
    }

    const std::uint64_t end = static_cast<std::uint64_t>(offset) + size;
    if (end > limit) {
        return false;
    }

    if (end > rom->size()) {
        rom->resize(static_cast<std::size_t>(end), 0);
    }
    if (size > 0) {
        std::memcpy(rom->data() + offset, data, size);
    }
    return true;
}

/// Walks one program image's chunks.
///
/// A trailing run shorter than a header is the end of the image rather than a
/// fault -- upstream stops there too -- but a chunk claiming more data than
/// remains is a truncated file and is refused.
[[nodiscard]] bool uploadProgram(QsfRoms& roms, const std::vector<std::uint8_t>& exe) {
    std::size_t pos = 0;
    while (exe.size() - pos >= kChunkHeaderSize) {
        const std::uint8_t* p = exe.data() + pos;

        const std::string_view tag(reinterpret_cast<const char*>(p), 3);
        const std::uint32_t    offset = readLe32(p + 3);
        const std::uint32_t    size   = readLe32(p + 7);
        pos += kChunkHeaderSize;

        if (size > exe.size() - pos) {
            return false;
        }
        if (!uploadSection(roms, tag, offset, exe.data() + pos, size)) {
            return false;
        }
        pos += size;
    }
    return true;
}

/// Owns the emulator state. HighlyQuixotic hands out a plain block whose size
/// it computes, so there is nothing to destroy beyond freeing it.
struct QsoundState {
    explicit QsoundState(std::size_t bytes) : storage(new std::byte[bytes]) {}
    [[nodiscard]] void* get() const { return storage.get(); }
    std::unique_ptr<std::byte[]> storage;
};

class QsfDecoder final : public IDecoder {
public:
    ~QsfDecoder() override { QsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf || psf->version != kQsfVersion) {
            return false;
        }

        url_    = source->url();
        tags_   = psf->tags;
        volume_ = (psf->volume > 0.0) ? psf->volume : 1.0;

        double lengthSeconds = 0.0;
        double fadeSeconds   = 0.0;
        if (psf->length && *psf->length > 0.0) {
            lengthSeconds = *psf->length;
            fadeSeconds   = psf->fade.value_or(0.0);
        } else {
            lengthSeconds =
                (settings_ != nullptr) ? settings_->SynthDefaultSeconds() : 150.0;
            fadeSeconds =
                (settings_ != nullptr) ? settings_->SynthDefaultFadeSeconds() : 8.0;
            if (lengthSeconds < 0.0) {
                lengthSeconds = 150.0;
            }
            if (fadeSeconds < 0.0) {
                fadeSeconds = 0.0;
            }
        }

        fadeStart_   = toFrames(lengthSeconds);
        totalFrames_ = toFrames(lengthSeconds + std::max(0.0, fadeSeconds));

        format_.sampleRate    = static_cast<double>(kSampleRate);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "QSF";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        // Asked per read, not latched at open: the listener can switch
        // repeat-one on part-way through a piece of game music and expects the
        // fade to stop coming.
        const bool endless = loopForever(settings_);
        if ((!endless && framePos_ >= totalFrames_) || !start()) {
            return false;
        }

        // Endless means exactly that: nothing bounds the read, so the rip keeps
        // being rendered past the length this player invented for it.
        const auto want =
            endless ? static_cast<std::size_t>(kFramesPerRead)
                    : static_cast<std::size_t>(std::min<std::int64_t>(
                          static_cast<std::int64_t>(kFramesPerRead),
                          totalFrames_ - framePos_));
        scratch_.resize(want * kChannels);
        const std::size_t got = render(scratch_.data(), want);
        if (got == 0) {
            return false;
        }

        applyGain(scratch_.data(), got);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp =
            static_cast<double>(framePos_) / static_cast<double>(kSampleRate);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);
        std::memcpy(dst, scratch_.data(), got * kChannels * sizeof(std::int16_t));
        out.setFrameCount(got);

        framePos_ += static_cast<std::int64_t>(got);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_ || !state_) {
            state_.reset();
            framePos_ = 0;
        }
        if (!start()) {
            return -1;
        }

        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       frame - framePos_));
            if (render(nullptr, step) == 0) {
                return -1;
            }
            framePos_ += static_cast<std::int64_t>(step);
        }
        return framePos_;
    }

    void close() override {
        // The state first: it holds the ROM pointers.
        state_.reset();
        roms_ = QsfRoms{};
    }

private:
    [[nodiscard]] bool start() {
        if (state_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, kQsfVersion);
        if (!psf || psf->empty()) {
            return false;
        }

        QsfRoms roms;
        for (const codecs::PsfProgram& program : psf->programs) {
            if (program.exe.empty()) {
                continue;
            }
            if (!uploadProgram(roms, program.exe)) {
                return false;
            }
        }
        if (roms.z80.empty()) {
            // No sound program, nothing to run. The sample ROM may legitimately
            // be absent -- a track can be pure synthesis -- but the Z80 cannot.
            return false;
        }

        initQsoundOnce();

        auto state = std::make_unique<QsoundState>(qsound_get_state_size());
        qsound_clear_state(state->get());

        if (roms.key.size() == kKeySize) {
            const std::uint8_t* k = roms.key.data();
            qsound_set_kabuki_key(state->get(), readBe32(k), readBe32(k + 4),
                                  readBe16(k + 8), k[10]);
        } else {
            // Including a KEY section of the wrong length, which upstream also
            // treats as absent rather than as an error.
            qsound_set_kabuki_key(state->get(), 0, 0, 0, 0);
        }

        roms_ = std::move(roms);
        qsound_set_z80_rom(state->get(), roms_.z80.data(),
                           static_cast<std::uint32_t>(roms_.z80.size()));
        qsound_set_sample_rom(state->get(), roms_.samples.data(),
                              static_cast<std::uint32_t>(roms_.samples.size()));

        state_ = std::move(state);
        return true;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) *
                                                      static_cast<double>(kSampleRate)));
    }

    /// qsound_execute() fills as much as it can and reports how much through
    /// the same variable. A null buffer discards, which is what seeking wants.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        std::size_t filled = 0;
        while (filled < frames) {
            auto howMany = static_cast<std::uint32_t>(frames - filled);
            const std::int32_t result =
                qsound_execute(state_->get(), 0x7fffffff,
                               out != nullptr ? out + filled * kChannels : nullptr,
                               &howMany);
            if (result < 0 || howMany == 0) {
                break;
            }
            filled += howMany;
        }
        return filled;
    }

    void applyGain(std::int16_t* frames, std::size_t count) {
        // No fade while looping for ever -- the fade is the thing that turns a
        // rip which never ends into a track that does, and repeat-one is the
        // listener saying they did not want that.
        const std::int64_t fadeLength =
            loopForever(settings_) ? 0 : totalFrames_ - fadeStart_;
        const bool         fading =
            fadeLength > 0 && framePos_ + static_cast<std::int64_t>(count) > fadeStart_;
        if (!fading && volume_ == 1.0) {
            return;
        }

        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            double             gain     = volume_;
            if (fadeLength > 0 && position > fadeStart_) {
                gain *= static_cast<double>(totalFrames_ - position) /
                        static_cast<double>(fadeLength);
            }
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                const double scaled =
                    static_cast<double>(frames[i * kChannels + channel]) * gain;
                frames[i * kChannels + channel] =
                    static_cast<std::int16_t>(std::clamp(scaled, -32768.0, 32767.0));
            }
        }
    }

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    Url                          url_;
    QsfRoms                      roms_;
    std::unique_ptr<QsoundState> state_;
    AudioFormat                  format_{};
    std::int64_t                 framePos_    = 0;
    std::int64_t                 fadeStart_   = 0;
    std::int64_t                 totalFrames_ = 0;
    double                       volume_      = 1.0;
    MetadataMap                  tags_;

    std::vector<std::int16_t> scratch_;
};

constexpr std::string_view kExtensions[] = {"qsf", "miniqsf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_qsf(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "QsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::QsfDecoder>();
        },
        .available = nullptr,
    });
}
