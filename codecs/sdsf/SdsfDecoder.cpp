// Saturn and Dreamcast rips, through HighlyTheoretical. The fifth of
// HighlyComplete's cores, and the only one that serves two formats.
//
// Port of the `type == 0x11 || type == 0x12` paths of Cog
// Plugins/HighlyComplete/HCDecoder.mm.
//
// SSF is Saturn (`0x11`) and DSF is Dreamcast (`0x12`), and one library plays
// both because both machines drive the same Yamaha sound chip -- the SCSP on
// Saturn and the AICA on Dreamcast are the same design, and `yam.c` is it. What
// differs is the processor feeding it: a 68000 on Saturn, an ARM7 on Dreamcast.
// `sega_get_state_size(version - 0x10)` picks between them, so the version byte
// is not a check here but the actual switch.
//
// The section format is a fourth variant. Where GSF puts a header on each
// section and SNSF derives a base from the first, an SSF/DSF section is a
// four-byte little-endian load address followed by data, and sections merge
// into one image that grows at *either* end -- a later section starting before
// everything seen so far shifts the whole buffer up and re-bases it.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include <Core/sega.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::uint8_t kSaturnVersion    = 0x11;  // ssf, minissf
constexpr std::uint8_t kDreamcastVersion = 0x12;  // dsf, minidsf

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// Both machines' sound hardware is clocked to produce 44.1 kHz, which is why
/// Cog leaves its default in place for these two types rather than overriding
/// it as it does for GSF, SNSF and 2SF.
constexpr int kSampleRate = 44100;

/// How much address space each machine's sound RAM covers. A section reaching
/// past it is truncated rather than refused, which is Cog's behaviour: rips
/// exist whose final section overruns by a few bytes.
constexpr std::size_t kSaturnSoundRam    = 0x80000;   // 512 KB
constexpr std::size_t kDreamcastSoundRam = 0x800000;  // 8 MB

/// The image is addressed within this window; the top bits of a load address
/// are the region and not part of the offset.
constexpr std::uint32_t kAddressMask = 0x7fffff;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeLe32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
}

/// sega_init() sets up tables shared by every emulator state, so it happens
/// once for the process rather than per track. Cog does the same from its
/// +initialize.
void initSegaOnce() {
    static std::once_flag once;
    std::call_once(once, [] { sega_init(); });
}

/// Merges one section into the image.
///
/// The image is `[4-byte load address][data]`, and the address can move: a
/// section that starts *earlier* than everything merged so far shifts the
/// existing data up, zero-fills the gap, and rewrites the header. That is what
/// makes this different from the other cores' loaders, where the origin is
/// fixed by the first section and later ones only ever write within it.
[[nodiscard]] bool mergeSection(std::vector<std::uint8_t>& image,
                                const std::vector<std::uint8_t>& exe,
                                std::size_t soundRam) {
    if (exe.size() < 4) {
        return false;
    }

    if (image.size() < 4) {
        image = exe;
        return true;
    }

    std::uint32_t imageStart = readLe32(image.data()) & kAddressMask;
    const std::uint32_t sectionStart = readLe32(exe.data()) & kAddressMask;

    std::size_t imageLength   = std::min<std::size_t>(image.size() - 4, soundRam);
    const std::size_t sectionLength = std::min<std::size_t>(exe.size() - 4, soundRam);

    if (sectionStart < imageStart) {
        const std::uint32_t shift = imageStart - sectionStart;
        image.resize(imageLength + 4 + shift, 0);
        std::memmove(image.data() + 4 + shift, image.data() + 4, imageLength);
        std::memset(image.data() + 4, 0, shift);
        imageLength += shift;
        imageStart = sectionStart;
        writeLe32(image.data(), imageStart);
    }

    if (sectionStart + sectionLength > imageStart + imageLength) {
        const std::size_t grow =
            (sectionStart + sectionLength) - (imageStart + imageLength);
        image.resize(imageLength + 4 + grow, 0);
        std::memset(image.data() + 4 + imageLength, 0, grow);
        imageLength += grow;
    }

    std::memcpy(image.data() + 4 + (sectionStart - imageStart), exe.data() + 4,
                sectionLength);
    return true;
}

/// Owns the emulator state. HighlyTheoretical hands out a plain block whose
/// size it computes, so there is nothing to destroy beyond freeing it.
struct SegaState {
    explicit SegaState(std::size_t bytes) : storage(new std::byte[bytes]) {}
    [[nodiscard]] void* get() const { return storage.get(); }
    std::unique_ptr<std::byte[]> storage;
};

class SdsfDecoder final : public IDecoder {
public:
    ~SdsfDecoder() override { SdsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf ||
            (psf->version != kSaturnVersion && psf->version != kDreamcastVersion)) {
            return false;
        }

        version_ = psf->version;
        url_     = source->url();
        tags_    = psf->tags;
        volume_  = (psf->volume > 0.0) ? psf->volume : 1.0;

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
        props.codec       = (version_ == kDreamcastVersion) ? "DSF" : "SSF";
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

    void close() override { state_.reset(); }

private:
    [[nodiscard]] bool start() {
        if (state_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, version_);
        if (!psf || psf->empty()) {
            return false;
        }

        const std::size_t soundRam = (version_ == kDreamcastVersion)
                                         ? kDreamcastSoundRam
                                         : kSaturnSoundRam;

        std::vector<std::uint8_t> image;
        for (const codecs::PsfProgram& program : psf->programs) {
            if (program.exe.empty()) {
                continue;
            }
            if (!mergeSection(image, program.exe, soundRam)) {
                return false;
            }
        }
        if (image.size() < 4) {
            return false;
        }

        initSegaOnce();

        const std::uint8_t machine = static_cast<std::uint8_t>(version_ - 0x10);
        auto state = std::make_unique<SegaState>(sega_get_state_size(machine));
        sega_clear_state(state->get(), machine);

        // Dry output and the DSP both on, and the DSP's dynamic recompiler off.
        // Cog makes the same three calls: the DSP is where a Saturn or
        // Dreamcast soundtrack's reverb and filtering live, so switching it off
        // would play the notes and not the mix -- and its recompiler is the
        // usual writable-and-executable-memory problem, declined here as it is
        // in every other core in this tree.
        sega_enable_dry(state->get(), 1);
        sega_enable_dsp(state->get(), 1);
        sega_enable_dsp_dynarec(state->get(), 0);

        // Truncate a section that runs past the machine's sound RAM rather than
        // refusing it. The four bytes are the load address, which is not part of
        // the payload.
        const std::uint32_t loadAddress = readLe32(image.data());
        std::size_t         length      = image.size();
        if (loadAddress + (length - 4) > soundRam) {
            length = soundRam - loadAddress + 4;
        }

        if (sega_upload_program(state->get(), image.data(),
                                static_cast<std::uint32_t>(length)) < 0) {
            return false;
        }

        state_ = std::move(state);
        return true;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) *
                                                      static_cast<double>(kSampleRate)));
    }

    /// sega_execute() fills as much as it can and reports how much through the
    /// same variable. A null buffer discards, which is what seeking wants.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        std::size_t filled = 0;
        while (filled < frames) {
            auto howMany = static_cast<std::uint32_t>(frames - filled);
            const std::int32_t result =
                sega_execute(state_->get(), 0x7fffffff,
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

    Url                        url_;
    std::unique_ptr<SegaState> state_;
    AudioFormat                format_{};
    std::uint8_t               version_     = 0;
    std::int64_t               framePos_    = 0;
    std::int64_t               fadeStart_   = 0;
    std::int64_t               totalFrames_ = 0;
    double                     volume_      = 1.0;
    MetadataMap                tags_;

    std::vector<std::int16_t> scratch_;
};

constexpr std::string_view kExtensions[] = {"ssf", "minissf", "dsf", "minidsf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_sdsf(xpcog::PluginRegistry& r) {
    // One decoder, four extensions, two consoles. The version byte in the file
    // decides which machine is built, so nothing here needs to.
    r.addDecoder({
        .name       = "SdsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::SdsfDecoder>();
        },
        .available = nullptr,
    });
}
