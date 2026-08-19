// Game Boy Advance rips, through mGBA. The second of HighlyComplete's cores.
//
// Port of the `type == 0x22` paths of Cog Plugins/HighlyComplete/HCDecoder.mm.
// A GSF is closer to an ordinary program than a USF is: the `exe` section holds
// a fragment of GBA cartridge ROM with a twelve-byte header saying where in the
// address space it belongs, and a set's `.gsflib` carries the game's whole
// program while each `.minigsf` overlays a few bytes -- usually the song number.
// Build the ROM, hand it to mGBA as a cartridge, and record what comes out.
//
// Two things differ from the USF core next door, and both are forced:
//
//   * The program is in `exe`, not `reserved` -- the opposite of USF, and the
//     reason codecs/psf hands over both sections rather than guessing.
//
//   * **The core is started in open(), not lazily.** Everywhere else that would
//     be waste; here the sample rate is not knowable without it. See below.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
// The feature macros these headers are read under are NOT set here, and not by
// the headers either -- they come from INTERFACE_COMPILE_DEFINITIONS on the
// mGBA::mgba target in CMakeLists.txt. That file explains why, and it is worth
// reading before touching this include block: getting it wrong compiles, links,
// and then crashes on the first call through struct mCore.
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
}

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

/// The PSF version byte for GSF. Verified against Cog's HCDecoder.mm.
constexpr std::uint8_t kGsfVersion = 0x22;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// What Cog asks mGBA for, and the size of its audio buffer.
constexpr std::size_t kAudioBufferFrames = 2048;

/// A GBA cartridge is mapped at 0x08000000 and is at most 32 MB, so the offset
/// in a GSF header is masked into that window -- the rips carry the full
/// address, and only the low bits address the ROM image.
constexpr std::uint32_t kRomOffsetMask = 0x01ffffff;

/// Header of a GSF `exe` section: entry point, load offset, length.
constexpr std::size_t kGsfHeaderSize = 12;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// Rounds up to a power of two, which is how a GBA cartridge is sized and what
/// Cog's loader does. mGBA maps the image with a mask rather than a bound, so a
/// ROM of an awkward size would alias rather than fault.
[[nodiscard]] std::size_t roundUpPowerOfTwo(std::size_t value) {
    std::size_t rounded = 1;
    while (rounded < value) {
        rounded <<= 1;
    }
    return rounded;
}

/// Overlays one program section onto the cartridge image being assembled.
///
/// psflib hands the sections over highest priority first -- the deepest
/// `.gsflib`, then its dependents, then the file that was asked for -- and each
/// is copied at its own offset over whatever is already there. That is the whole
/// mechanism by which a 200-byte `.minigsf` selects a track: it overwrites the
/// handful of bytes the game reads as its song index.
[[nodiscard]] bool applyProgram(std::vector<std::uint8_t>& rom, std::size_t& romSize,
                                const std::vector<std::uint8_t>& exe) {
    if (exe.size() < kGsfHeaderSize) {
        return false;
    }

    const std::uint32_t offset  = readLe32(exe.data() + 4) & kRomOffsetMask;
    const std::uint32_t claimed = readLe32(exe.data() + 8);
    const std::size_t   payload = exe.size() - kGsfHeaderSize;

    // A header claiming less than it carries is malformed. Claiming *more* is
    // tolerated and clamped: Cog trusts the claim and memcpy()s that many bytes
    // out of a shorter buffer, which reads past the end of the inflated
    // section. Nothing in this corpus does it, but a rip is an untrusted file.
    if (claimed < payload) {
        return false;
    }
    const std::size_t length = payload;

    const std::size_t needed = static_cast<std::size_t>(offset) + claimed;
    if (needed > (std::size_t{32} << 20)) {
        return false;  // larger than any GBA cartridge
    }
    if (romSize < needed) {
        // Two sizes, and conflating them is a segfault rather than a wrong
        // note. `romSize` -- a power of two -- is what mGBA is told the
        // cartridge is, because it masks ROM addresses against that size rather
        // than bounds-checking them; give it a size that is not a power of two
        // and an ordinary read lands outside the buffer. The extra ten bytes
        // are allocated beyond that and never declared: the ARM7 prefetches
        // ahead of the program counter, so a read can run just past the end of
        // a ROM the game never meant to reach. Cog allocates `rsize + 10` and
        // passes `rsize`, and the difference is easy to lose in the copying.
        romSize = roundUpPowerOfTwo(needed);
        rom.resize(romSize + 10, 0);
    }

    std::memcpy(rom.data() + offset, exe.data() + kGsfHeaderSize, length);
    return true;
}

/// mGBA logs to stdout by default -- a rip that pokes an unmapped register
/// would otherwise print for every frame of a track. Cog installs the same
/// no-op. The logger is global to mGBA rather than per core, so this is done
/// once for the process.
void silenceMgbaLogging() {
    static mLogger logger = {};
    static const bool once = [] {
        logger.log = [](mLogger*, int, mLogLevel, const char*, va_list) {};
        mLogSetDefaultLogger(&logger);
        return true;
    }();
    (void)once;
}

/// Shared with the GME and USF decoders: Cog's synthSampleRate clamp.
constexpr int kDefaultRate = 44100;

/// Owns the emulator and the cartridge image behind it, in that order of
/// teardown. mGBA's VFile wraps the ROM bytes without copying them, so the
/// vector has to outlive the core that is reading from it.
struct GbaCore {
    ~GbaCore() {
        if (core != nullptr) {
            mCoreConfigDeinit(&core->config);
            core->deinit(core);
        }
    }
    GbaCore()                          = default;
    GbaCore(const GbaCore&)            = delete;
    GbaCore& operator=(const GbaCore&) = delete;

    std::vector<std::uint8_t> rom;      ///< romSize bytes, plus prefetch slack
    std::size_t               romSize = 0;  ///< what mGBA is told, a power of two
    mCore*                    core    = nullptr;
};

class GsfDecoder final : public IDecoder {
public:
    ~GsfDecoder() override { GsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    /// Unlike the USF core, this starts the emulator here rather than on the
    /// first frame wanted, and the reason is the sample rate.
    ///
    /// A GBA does not have one. The rate the sound hardware runs at is
    /// `0x200 >> SOUNDBIAS.resolution` cycles per sample, and SOUNDBIAS is a
    /// register the *game* writes during its own initialisation -- so the
    /// answer does not exist until the ROM has been loaded, reset, and run far
    /// enough to configure its audio. mGBA reports it through
    /// `core->audioSampleRate()`, and there is nothing in the tag block that
    /// implies it.
    ///
    /// Cog sidesteps this by declaring a constant 65536 with an `// XXX` beside
    /// it and the `audioSampleRate()` call left commented out. That is right for
    /// the games that select the 64 KHz rate and an octave high for the ones
    /// that keep the 32 KHz default, so it is worth the cost of starting early.
    /// Scanning a GSF set is correspondingly dearer than scanning a USF one.
    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(source->url(), *registry_, kGsfVersion);
        if (!psf || psf->empty()) {
            return false;
        }

        auto gba = std::make_unique<GbaCore>();
        for (const codecs::PsfProgram& program : psf->programs) {
            // GSF is the mirror image of USF: the program is in `exe`, and
            // `reserved` is unused. A file with an empty `exe` carries no ROM.
            if (program.exe.empty()) {
                continue;
            }
            if (!applyProgram(gba->rom, gba->romSize, program.exe)) {
                return false;
            }
        }
        if (gba->romSize == 0) {
            return false;
        }

        silenceMgbaLogging();

        // Borrowed, not copied -- gba->rom must outlive the core, which is what
        // GbaCore exists to guarantee. mCore takes ownership of the VFile and
        // closes it in deinit().
        VFile* rom = VFileFromConstMemory(gba->rom.data(), gba->romSize);
        if (rom == nullptr) {
            return false;
        }

        gba->core = mCoreFindVF(rom);
        if (gba->core == nullptr) {
            rom->close(rom);
            return false;
        }
        gba->core->init(gba->core);
        mCoreInitConfig(gba->core, nullptr);
        gba->core->setAudioBufferSize(gba->core, kAudioBufferFrames);

        // No BIOS: a rip is not entitled to one and mGBA's high-level
        // replacement is what every GSF player uses. `volume` here is mGBA's
        // own mixer level, 0x100 being unity, and is separate from the PSF
        // `volume` tag applied further down.
        mCoreOptions options = {};
        options.skipBios   = true;
        options.useBios    = false;
        options.sampleRate = 32768;
        options.volume     = 0x100;
        mCoreConfigLoadDefaults(&gba->core->config, &options);

        gba->core->loadROM(gba->core, rom);
        gba->core->reset(gba->core);

        core_ = std::move(gba);

        // Run until the sound hardware has produced something. Until then
        // SOUNDBIAS holds its reset value and audioSampleRate() would answer
        // for a machine the game has not finished configuring.
        mAudioBuffer* buffer = core_->core->getAudioBuffer(core_->core);
        for (int frame = 0; frame < kMaxPrimeFrames && mAudioBufferAvailable(buffer) == 0;
             ++frame) {
            core_->core->runFrame(core_->core);
        }

        const unsigned reported = core_->core->audioSampleRate(core_->core);
        rate_ = (reported >= kMinRate && reported <= kMaxRate)
                    ? static_cast<int>(reported)
                    : kDefaultRate;

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

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        url_      = source->url();
        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "GSF";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        if (!core_ || framePos_ >= totalFrames_) {
            return false;
        }

        const auto want = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
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

    /// A GBA cannot be rewound either, so seeking backwards means resetting the
    /// machine and running it forward with the audio thrown away. Forwards just
    /// runs on from where it is, which is why the direction is tested: Cog does
    /// the same, and on a long track the difference is seconds.
    std::int64_t seek(std::int64_t frame) override {
        if (!core_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_) {
            core_->core->reset(core_->core);
            framePos_ = 0;
        }

        std::vector<std::int16_t> discard(kFramesPerRead * kChannels);
        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       frame - framePos_));
            const std::size_t got = render(discard.data(), step);
            if (got == 0) {
                return -1;
            }
            framePos_ += static_cast<std::int64_t>(got);
        }
        return framePos_;
    }

    void close() override { core_.reset(); }

private:
    /// mGBA has no fixed frames-per-call: it fills an internal buffer as the
    /// emulated hardware produces samples, so this drains what is there and
    /// runs whole video frames until there is more. `kMaxPrimeFrames` bounds
    /// the wait -- a rip that has crashed its own CPU produces nothing, for
    /// ever, and a decoder that never returns is worse than one that stops.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        mAudioBuffer* buffer = core_->core->getAudioBuffer(core_->core);
        std::size_t   filled = 0;

        while (filled < frames) {
            const std::size_t got =
                mAudioBufferRead(buffer, out + filled * kChannels, frames - filled);
            filled += got;
            if (filled >= frames) {
                break;
            }

            int spun = 0;
            while (mAudioBufferAvailable(buffer) == 0 && spun < kMaxPrimeFrames) {
                core_->core->runFrame(core_->core);
                ++spun;
            }
            if (spun >= kMaxPrimeFrames) {
                break;
            }
        }
        return filled;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(
            std::llround(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    /// The fade and the `volume` tag, in one pass. Identical in shape to the USF
    /// core's: `length` is the only thing that ends a PSF track, so the fade is
    /// the decoder's job rather than the chain's.
    void applyGain(std::int16_t* frames, std::size_t count) {
        const std::int64_t fadeLength = totalFrames_ - fadeStart_;
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

    /// Sixty video frames is a second of emulated time with nothing to show for
    /// it, which is long enough to call a rip broken rather than slow.
    static constexpr int kMaxPrimeFrames = 60;
    static constexpr unsigned kMinRate   = 8000;
    static constexpr unsigned kMaxRate   = 192000;

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    Url                      url_;
    std::unique_ptr<GbaCore> core_;
    AudioFormat              format_{};
    int                      rate_        = kDefaultRate;
    std::int64_t             framePos_    = 0;
    std::int64_t             fadeStart_   = 0;
    std::int64_t             totalFrames_ = 0;
    double                   volume_      = 1.0;
    MetadataMap              tags_;

    std::vector<std::int16_t> scratch_;
};

constexpr std::string_view kExtensions[] = {"gsf", "minigsf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_gsf(xpcog::PluginRegistry& r) {
    // `gsflib` is deliberately absent, for the same reason `usflib` is: it holds
    // the game's whole program and no track.
    r.addDecoder({
        .name       = "GsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::GsfDecoder>();
        },
        .available = nullptr,
    });
}
