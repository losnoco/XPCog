// Super Nintendo rips, through snes9x. The fourth of HighlyComplete's cores.
//
// Port of the `type == 0x23` paths of Cog Plugins/HighlyComplete/HCDecoder.mm.
//
// An SNSF is a cartridge image and, optionally, save RAM. That it needs a whole
// SNES rather than just a sound chip is the point of the format: the SPC700 has
// 64 KB of audio RAM, and there are tracks that do not fit in it at any
// compression. Tales of Phantasia's vocal theme is the canonical one -- a sung
// verse cannot live in 64 KB even as BRR, so Wolf Team's driver streams sample
// chunks from cartridge ROM through the CPU-APU I/O ports while the music
// plays, under a driver computing sixteen virtual voices and sounding the
// loudest eight.
//
// An `.spc` dump is a snapshot of those 64 KB and therefore *cannot* contain
// such a track. That is why SNSF exists, and it makes the CPU-APU handshake the
// thing this decoder has to keep honest: if the emulated CPU falls behind, the
// stream starves and the vocal breaks up while every other track in the set
// plays perfectly.
//
// The section format differs again from its siblings. GSF has one header per
// section; 2SF has offset/length chunks in both sections; SNSF takes the
// **first** section's offset as a base that every later section is biased by,
// then masks the result into the cartridge window.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

// snes9x.h first, and not alphabetically: memmap.h uses ROM_NAME_LEN and other
// constants it does not define itself, so it only parses after snes9x.h has
// been read. Sorting these breaks the build.
#include <snes9x/snes9x.h>

#include <snes9x/apu.h>
#include <snes9x/cpuexec.h>
#include <snes9x/memmap.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// The PSF version byte for SNSF. Verified against Cog's HCDecoder.mm.
constexpr std::uint8_t kSnsfVersion = 0x23;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// What Cog asks snes9x to produce. The S-DSP itself runs at 32 kHz, so this is
/// the rate the hardware makes rather than a resampling choice.
constexpr int kSampleRate = 32000;

/// Cog's `InterpolationMethod = 2`. The S-DSP interpolates its BRR samples with
/// a 4-tap Gaussian filter, and reproducing that is what makes a rip sound like
/// the console rather than like a cleaner version of it.
constexpr int kGaussianInterpolation = 2;

/// Cartridge addresses are masked into this window, which is what Cog's loader
/// does after applying the base.
constexpr std::uint32_t kRomMask = 0x1fffffff;

/// Save RAM is 128 KB, erased to 0xff -- the state a battery-backed SRAM is in
/// before a game has written to it.
constexpr std::size_t   kSramSize = 0x20000;
constexpr std::uint8_t  kSramErased = 0xff;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// The cartridge and its save RAM, assembled from the chain.
struct Cartridge {
    std::vector<std::uint8_t> rom;
    std::vector<std::uint8_t> sram;
    bool                      haveBase = false;
    std::uint32_t             base     = 0;
};

/// One `exe` section: offset, length, data.
///
/// The base is the part that is peculiar to SNSF. The first section the chain
/// hands over -- the deepest `.snsflib`, since psflib reports highest priority
/// first -- sets the origin, and every section after it is placed relative to
/// that rather than absolutely. A `.minisnsf` of fifty-odd bytes then rewrites
/// the handful that select the track.
[[nodiscard]] bool applySection(Cartridge& cart, const std::uint8_t* data,
                                std::size_t size) {
    if (size < 8) {
        return false;
    }

    std::uint32_t      offset = readLe32(data + 0);
    const std::uint32_t length = readLe32(data + 4);
    if (length > size - 8) {
        return false;  // states more than it carries
    }

    if (!cart.haveBase) {
        cart.haveBase = true;
        cart.base     = offset;
    } else {
        offset += cart.base;
    }
    offset &= kRomMask;

    const std::size_t needed = static_cast<std::size_t>(offset) + length;
    if (needed > (std::size_t{64} << 20)) {
        return false;  // larger than any SNES cartridge
    }
    if (cart.rom.size() < needed) {
        cart.rom.resize(needed, 0);
    }
    std::memcpy(cart.rom.data() + offset, data + 8, length);
    return true;
}

/// The `reserved` section: type/length records, where type 0 is save RAM. The
/// record's payload begins with the offset to write at.
[[nodiscard]] bool applySave(Cartridge& cart, const std::vector<std::uint8_t>& reserved) {
    std::size_t position = 0;
    while (position + 8 < reserved.size()) {
        const std::uint32_t type   = readLe32(reserved.data() + position);
        const std::uint32_t length = readLe32(reserved.data() + position + 4);
        if (type == 0) {
            if (position + 8 + length > reserved.size()) {
                return false;
            }
            if (cart.sram.empty()) {
                cart.sram.assign(kSramSize, kSramErased);
            }
            if (length > 4) {
                const std::uint32_t offset = readLe32(reserved.data() + position + 8);
                if (offset < cart.sram.size()) {
                    const std::size_t room = cart.sram.size() - offset;
                    const std::size_t take = std::min<std::size_t>(length - 4, room);
                    std::memcpy(cart.sram.data() + offset,
                                reserved.data() + position + 12, take);
                }
            }
        }
        position += length + 8;
    }
    return true;
}

/// Owns the emulator state and tears it down in the order snes9x wants.
struct SnesCore {
    ~SnesCore() {
        if (started) {
            S9xReset(&state);
            state.Memory.Deinit();
            S9xDeinitAPU(&state);
        }
    }
    SnesCore()                           = default;
    SnesCore(const SnesCore&)            = delete;
    SnesCore& operator=(const SnesCore&) = delete;

    S9xState state{};
    bool     started = false;
};

class SnsfDecoder final : public IDecoder {
public:
    ~SnsfDecoder() override { SnsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    /// Tags only; the SNES waits for the first frame anyone wants. The S-DSP
    /// rate is fixed by the hardware, so nothing here needs the machine running
    /// -- the same reasoning that keeps the USF and 2SF cores lazy, and the
    /// reason a library scan of a 373-track set does not boot 373 consoles.
    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf || psf->version != kSnsfVersion) {
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
        props.codec       = "SNSF";
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

        // Backwards means a fresh console: snes9x has no rewind here.
        if (frame < framePos_ || !core_) {
            core_.reset();
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
        core_.reset();
        pending_.clear();
        pendingPos_ = 0;
    }

private:
    /// Assembles the cartridge and boots the console, once.
    [[nodiscard]] bool start() {
        if (core_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, kSnsfVersion);
        if (!psf || psf->empty()) {
            return false;
        }

        Cartridge cart;
        for (const codecs::PsfProgram& program : psf->programs) {
            if (!program.reserved.empty() && !applySave(cart, program.reserved)) {
                return false;
            }
            if (!program.exe.empty() &&
                !applySection(cart, program.exe.data(), program.exe.size())) {
                return false;
            }
        }
        if (cart.rom.empty()) {
            return false;
        }

        auto core = std::make_unique<SnesCore>();
        S9xState* state = &core->state;

        // SoundSync is what makes this core suitable for a rip at all: it ties
        // the CPU to the sound output rather than to a video frame rate, so the
        // APU is never starved by a frame running long. For a track that
        // streams its samples through the CPU that is the difference between
        // music and stuttering.
        state->Settings.SoundSync           = true;
        state->Settings.Mute                = false;
        state->Settings.SoundPlaybackRate   = kSampleRate;
        state->Settings.InterpolationMethod = kGaussianInterpolation;

        if (!state->Memory.Init(state)) {
            return false;
        }
        core->started = true;

        if (!S9xInitAPU(state)) {
            return false;
        }
        S9xInitSound(state, 10);

        if (!state->Memory.LoadROMSNSF(
                cart.rom.data(), static_cast<std::int32_t>(cart.rom.size()),
                cart.sram.empty() ? nullptr : cart.sram.data(),
                static_cast<std::int32_t>(cart.sram.size()))) {
            return false;
        }
        S9xSetSoundMute(state, false);

        core_ = std::move(core);
        return true;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) *
                                                      static_cast<double>(kSampleRate)));
    }

    /// snes9x produces whatever the last slice of emulation happened to make,
    /// so this keeps a remainder between calls and runs the CPU when it comes
    /// up short. A null destination discards, which is what seeking wants.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        std::size_t filled = 0;
        int         spun   = 0;

        while (filled < frames) {
            const std::size_t have = (pending_.size() / kChannels) - pendingPos_;
            if (have > 0) {
                const std::size_t take = std::min(frames - filled, have);
                if (out != nullptr) {
                    std::memcpy(out + filled * kChannels,
                                pending_.data() + pendingPos_ * kChannels,
                                take * kChannels * sizeof(std::int16_t));
                }
                pendingPos_ += take;
                filled += take;
                continue;
            }

            pending_.clear();
            pendingPos_ = 0;
            if (spun >= kMaxSpins) {
                break;
            }

            S9xState* state = &core_->state;
            S9xSyncSound(state);
            S9xMainLoop(state);

            // Sample count is per channel pair already interleaved; the & ~1
            // keeps it to whole frames, as Cog's `& ~3` does in bytes.
            const int available = S9xGetSampleCount(state) & ~1;
            if (available <= 0) {
                ++spun;
                continue;
            }
            spun = 0;
            pending_.assign(static_cast<std::size_t>(available), 0);
            S9xMixSamples(state, reinterpret_cast<std::uint8_t*>(pending_.data()),
                          available);
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

    /// Runs of the main loop that produce nothing at all before the rip is
    /// called broken rather than quiet.
    static constexpr int kMaxSpins = 600;

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    Url                       url_;
    std::unique_ptr<SnesCore> core_;
    AudioFormat               format_{};
    std::int64_t              framePos_    = 0;
    std::int64_t              fadeStart_   = 0;
    std::int64_t              totalFrames_ = 0;
    double                    volume_      = 1.0;
    MetadataMap               tags_;

    std::vector<std::int16_t> scratch_;
    std::vector<std::int16_t> pending_;
    std::size_t               pendingPos_ = 0;
};

constexpr std::string_view kExtensions[] = {"snsf", "minisnsf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_snsf(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "SnsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::SnsfDecoder>();
        },
        .available = nullptr,
    });
}
