// PlayStation and PlayStation 2 rips, through HighlyExperimental. The seventh
// of HighlyComplete's cores, and the format the whole family is named after.
//
// Port of the `type == 1 || type == 2` paths of Cog
// Plugins/HighlyComplete/HCDecoder.mm.
//
// One core, two consoles, as HighlyTheoretical is for Saturn and Dreamcast:
// HighlyExperimental emulates the PS2's IOP -- an R3000 driving the SPU2 -- and
// drops it into PS1 compatibility mode for PSF, so `psx_get_state_size(1)`
// builds a PlayStation and `(2)` a PlayStation 2.
//
// The two formats differ more than the other pairs do, and the difference is
// what `psf2fs.c` has been sitting in vendor/psflib for since stage 0:
//
//   * A **PSF** carries a PS-EXE in `exe` -- a 0x800-byte header stating a load
//     address, entry point and stack pointer, then the code -- and the loader
//     uploads it into IOP RAM.
//   * A **PSF2** carries no executable at all. Its sections are a *filesystem*,
//     and the emulator reads files out of it on demand through a callback while
//     it runs. psf2fs assembles that filesystem from the chain, and
//     `psx_set_readfile()` hands the emulator the way in.
//
// So PSF is "load a program and run it" and PSF2 is "boot a machine and let it
// mount this". Nothing else here works the second way.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include <Core/bios.h>
#include <Core/hebios.h>
#include <Core/iop.h>
#include <Core/psx.h>
#include <Core/r3000.h>

#include <psflib/psf2fs.h>
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

constexpr std::uint8_t kPlaystationVersion  = 0x01;  // psf, minipsf
constexpr std::uint8_t kPlaystation2Version = 0x02;  // psf2, minipsf2

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// The PS1's SPU runs at 44.1 kHz and the PS2's SPU2 at 48 kHz. Cog declares
/// the same two rates.
constexpr int kPs1SampleRate = 44100;
constexpr int kPs2SampleRate = 48000;

/// A PS-EXE header is 0x800 bytes, and the fields this needs sit at fixed
/// offsets inside it: `pc0` the entry point, `t_addr` where the text segment
/// loads, `s_ptr` the initial stack pointer.
constexpr std::size_t kExeHeaderSize = 0x800;
constexpr std::size_t kOffsetPc0     = 16;
constexpr std::size_t kOffsetTextAddr = 24;
constexpr std::size_t kOffsetStackPtr = 48;
/// The region string sits inside the 60-byte title field.
constexpr std::size_t kOffsetRegion = 113;

/// IOP RAM is 2 MB, and a PS-EXE has to land inside it above the kernel.
constexpr std::uint32_t kIopAddressMask = 0x1fffff;
constexpr std::uint32_t kIopKernelTop   = 0x10000;
constexpr std::uint32_t kIopRamSize     = 0x200000;
constexpr std::uint32_t kMaxExeSize     = 0x1f0000;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] bool startsWithIgnoringCase(const std::uint8_t* data, std::size_t size,
                                          std::string_view text) {
    if (size < text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto a = static_cast<char>(std::tolower(data[i]));
        const auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

/// The BIOS image is global to HighlyExperimental rather than per emulator, so
/// it is installed once and psx_init() runs once. Cog does both from its
/// +initialize.
void initPsxOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        bios_set_image(hebios, HEBIOS_SIZE);
        psx_init();
    });
}

/// Owns the emulator state, and for PSF2 the filesystem it reads through --
/// which must outlive it, since the emulator holds the pointer.
struct PsxState {
    PsxState(std::size_t bytes, void* filesystem)
        : storage(new std::byte[bytes]), fs(filesystem) {}
    ~PsxState() {
        if (fs != nullptr) {
            psf2fs_delete(fs);
        }
    }
    PsxState(const PsxState&)            = delete;
    PsxState& operator=(const PsxState&) = delete;

    [[nodiscard]] void* get() const { return storage.get(); }

    std::unique_ptr<std::byte[]> storage;
    void*                        fs = nullptr;
};

/// What the emulator calls to read a file out of a PSF2's filesystem.
int EMU_CALL virtualReadFile(void* context, const char* path, int offset, char* buffer,
                             int length) {
    return psf2fs_virtual_readfile(context, path, offset, buffer, length);
}

class PsxDecoder final : public IDecoder {
public:
    ~PsxDecoder() override { PsxDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf || (psf->version != kPlaystationVersion &&
                     psf->version != kPlaystation2Version)) {
            return false;
        }

        version_ = psf->version;
        rate_    = (version_ == kPlaystation2Version) ? kPs2SampleRate : kPs1SampleRate;
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

        format_.sampleRate    = static_cast<double>(rate_);
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
        props.codec       = (version_ == kPlaystation2Version) ? "PSF2" : "PSF";
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
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
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

        initPsxOnce();

        // Nested tags for `_refresh`, which states the region's frame rate and
        // belongs to the game, so a set puts it in the library.
        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, version_, /*wantNestedTags=*/true);
        if (!psf || psf->empty()) {
            return false;
        }

        return (version_ == kPlaystation2Version) ? startPs2(*psf) : startPs1(*psf);
    }

    /// PSF: upload each PS-EXE into IOP RAM, in the order the chain gave them.
    [[nodiscard]] bool startPs1(const codecs::PsfFile& psf) {
        auto state = std::make_unique<PsxState>(psx_get_state_size(1), nullptr);
        psx_clear_state(state->get(), 1);

        void*    iop     = psx_get_iop_state(state->get());
        unsigned refresh = tagRefresh(psf.tags);
        bool     first   = true;

        for (const codecs::PsfProgram& program : psf.programs) {
            const std::vector<std::uint8_t>& exe = program.exe;
            if (exe.size() < kExeHeaderSize) {
                return false;
            }

            std::uint32_t address = readLe32(exe.data() + kOffsetTextAddr) &
                                    kIopAddressMask;
            const auto size = static_cast<std::uint32_t>(exe.size() - kExeHeaderSize);
            if (address < kIopKernelTop || size > kMaxExeSize ||
                address + size > kIopRamSize) {
                return false;
            }

            iop_upload_to_ram(iop, address, exe.data() + kExeHeaderSize, size);

            // The region is written into the title field, and it is the only
            // statement of the machine's frame rate when no `_refresh` tag says.
            if (refresh == 0 && exe.size() > kOffsetRegion + 13) {
                const std::uint8_t* region = exe.data() + kOffsetRegion;
                const std::size_t   room   = exe.size() - kOffsetRegion;
                if (startsWithIgnoringCase(region, room, "Japan")) {
                    refresh = 60;
                } else if (startsWithIgnoringCase(region, room, "Europe")) {
                    refresh = 50;
                } else if (startsWithIgnoringCase(region, room, "North America")) {
                    refresh = 60;
                }
            }

            // The *first* image the chain returns is the deepest library, and
            // its header is the one whose entry point and stack pointer the
            // machine starts from. Later overlays change memory, not where
            // execution begins.
            if (first) {
                void* r3000 = iop_get_r3000_state(iop);
                r3000_setreg(r3000, R3000_REG_PC, readLe32(exe.data() + kOffsetPc0));
                r3000_setreg(r3000, R3000_REG_GEN + 29,
                             readLe32(exe.data() + kOffsetStackPtr));
                first = false;
            }
        }
        if (first) {
            return false;  // nothing in the chain carried an executable
        }

        if (refresh != 0) {
            psx_set_refresh(state->get(), refresh);
        }
        state_ = std::move(state);
        return true;
    }

    /// PSF2: build the filesystem and let the machine read out of it.
    ///
    /// psf2fs wants the sections in psflib's own order, so the chain is walked
    /// again through psf2fs_load_callback rather than through the images
    /// codecs/psf already returned -- the callback assembles directories as it
    /// goes and there is no way to hand it an image after the fact.
    [[nodiscard]] bool startPs2(const codecs::PsfFile& psf) {
        void* fs = psf2fs_create();
        if (fs == nullptr) {
            return false;
        }

        for (const codecs::PsfProgram& program : psf.programs) {
            if (psf2fs_load_callback(fs, program.exe.data(), program.exe.size(),
                                     program.reserved.data(),
                                     program.reserved.size()) < 0) {
                psf2fs_delete(fs);
                return false;
            }
        }

        auto state = std::make_unique<PsxState>(psx_get_state_size(2), fs);
        psx_clear_state(state->get(), 2);

        const unsigned refresh = tagRefresh(psf.tags);
        if (refresh != 0) {
            psx_set_refresh(state->get(), refresh);
        }
        psx_set_readfile(state->get(), &virtualReadFile, fs);

        state_ = std::move(state);
        return true;
    }

    [[nodiscard]] static unsigned tagRefresh(const MetadataMap& tags) {
        const std::string_view text = tags.first("_refresh");
        if (text.empty()) {
            return 0;
        }
        try {
            return static_cast<unsigned>(std::stoul(std::string{text}));
        } catch (...) {
            return 0;
        }
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(
            std::llround(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        std::size_t filled = 0;
        while (filled < frames) {
            auto howMany = static_cast<std::uint32_t>(frames - filled);
            const std::int32_t result =
                psx_execute(state_->get(), 0x7fffffff,
                            out != nullptr ? out + filled * kChannels : nullptr,
                            &howMany, 0);
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

    Url                       url_;
    std::unique_ptr<PsxState> state_;
    AudioFormat               format_{};
    std::uint8_t              version_     = 0;
    int                       rate_        = kPs1SampleRate;
    std::int64_t              framePos_    = 0;
    std::int64_t              fadeStart_   = 0;
    std::int64_t              totalFrames_ = 0;
    double                    volume_      = 1.0;
    MetadataMap               tags_;

    std::vector<std::int16_t> scratch_;
};

constexpr std::string_view kExtensions[] = {"psf", "minipsf", "psf2", "minipsf2"};

}  // namespace
}  // namespace xpcog

void xpcog_register_psx(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "PsxDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::PsxDecoder>();
        },
        .available = nullptr,
    });
}
