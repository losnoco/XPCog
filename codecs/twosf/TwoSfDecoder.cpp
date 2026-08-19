// Nintendo DS rips, through melonDS. The third of HighlyComplete's cores.
//
// Port of the `type == 0x24` paths of Cog Plugins/HighlyComplete/HCDecoder.mm.
// A 2SF is a DS cartridge image: `exe` carries the ROM as a series of
// offset/length chunks, a set's `.2sflib` holds the game and each `.mini2sf`
// overlays the few bytes naming the track. Build the cartridge, boot a DS, and
// record what its SPU produces.
//
// Both sections are used, which no other core here does. `exe` is the ROM map;
// `reserved` holds zlib-compressed `SAVE` chunks in the same map format. Cog
// parses the save chunk and then never hands it to the emulator, and so does
// this -- see loadSave() for why it is still parsed.
//
// The core is melonDS, not the DeSmuME-derived vio2sf the name suggests; see
// vendor/vio2sf/CMakeLists.txt.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <vio2sf/NDS.h>
#include <vio2sf/NDSCart.h>
#include <vio2sf/SPI.h>
#include <vio2sf/SPU.h>

#include <zlib.h>

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

/// The PSF version byte for 2SF. Verified against Cog's HCDecoder.mm.
constexpr std::uint8_t kTwoSfVersion = 0x24;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// The DS SPU runs off the ARM7 clock divided by 1024. Not a round number, and
/// not something to round: 32728.5 Hz is what the hardware produces, and
/// declaring 32768 would drift a semitone-ish over a long track.
constexpr double kArm7Clock  = 33513982.0;
constexpr double kSampleRate = kArm7Clock / 1024.0;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::size_t roundUpPowerOfTwo(std::size_t value) {
    std::size_t rounded = 1;
    while (rounded < value) {
        rounded <<= 1;
    }
    return rounded;
}

/// A cartridge image being assembled, and the size to tell melonDS it is.
///
/// Two sizes again, for the same reason GSF has two: `declared` is rounded up
/// to a power of two because that is what a DS cartridge is, and `bytes` holds
/// ten more so a read just past the end lands in our buffer rather than
/// somewhere else. `declared` is what ParseROM() is given.
struct Image {
    std::vector<std::uint8_t> bytes;
    std::size_t               declared = 0;
};

/// One chunk of the 2SF map format: a 4-byte offset, a 4-byte length, then the
/// data. Sections arrive highest priority first and each overlays the last,
/// which is how a `.mini2sf` of a few hundred bytes selects a track.
///
/// `roundSize` is false for the save image: only the ROM is a cartridge.
[[nodiscard]] bool applyMap(Image& image, const std::uint8_t* data, std::size_t size,
                            bool roundSize) {
    if (size < 8) {
        return false;
    }

    const std::uint32_t offset  = readLe32(data + 0);
    const std::uint32_t claimed = readLe32(data + 4);
    const std::size_t   payload = size - 8;

    // Clamped rather than trusted. Cog copies `claimed` bytes out of a buffer
    // holding `payload`, which reads past the end of the inflated chunk when a
    // file says more than it carries. A rip is an untrusted file.
    const std::size_t length = std::min<std::size_t>(claimed, payload);

    const std::size_t needed = static_cast<std::size_t>(offset) + claimed;
    if (needed > (std::size_t{256} << 20)) {
        return false;  // larger than any DS cartridge
    }
    if (image.declared < needed) {
        image.declared = roundSize ? roundUpPowerOfTwo(needed) : needed;
        image.bytes.resize(image.declared + 10, 0);
    }

    std::memcpy(image.bytes.data() + offset, data + 8, length);
    return true;
}

/// Inflates one `SAVE` chunk and applies it. The chunk states its own
/// decompressed CRC, which is checked -- psflib checks the *compressed* CRC of
/// the section, and this is a second one inside it.
[[nodiscard]] bool applyMapZ(Image& image, const std::uint8_t* data, std::size_t size,
                             std::uint32_t expectedCrc) {
    std::vector<std::uint8_t> inflated(std::max<std::size_t>(size * 4, 4096));
    for (;;) {
        uLongf produced = static_cast<uLongf>(inflated.size());
        const int result = uncompress(inflated.data(), &produced, data,
                                      static_cast<uLong>(size));
        if (result == Z_OK) {
            inflated.resize(produced);
            break;
        }
        if (result != Z_BUF_ERROR && result != Z_MEM_ERROR) {
            return false;
        }
        if (inflated.size() > (std::size_t{256} << 20)) {
            return false;
        }
        inflated.resize(inflated.size() * 2);
    }

    const uLong actual = crc32(crc32(0L, Z_NULL, 0), inflated.data(),
                               static_cast<uInt>(inflated.size()));
    if (actual != expectedCrc) {
        return false;
    }
    return applyMap(image, inflated.data(), inflated.size(), /*roundSize=*/false);
}

/// The `reserved` section: a run of `SAVE` records, each with its own length
/// and CRC.
[[nodiscard]] bool loadSave(Image& image, const std::vector<std::uint8_t>& reserved) {
    if (reserved.size() < 16) {
        return false;
    }
    constexpr std::uint32_t kSaveMagic = 0x45564153;  // "SAVE", little-endian

    std::size_t position = 0;
    while (position + 12 < reserved.size()) {
        const std::uint32_t size = readLe32(reserved.data() + position + 4);
        const std::uint32_t crc  = readLe32(reserved.data() + position + 8);
        if (readLe32(reserved.data() + position) == kSaveMagic) {
            if (position + 12 + size > reserved.size()) {
                return false;
            }
            if (!applyMapZ(image, reserved.data() + position + 12, size, crc)) {
                return false;
            }
        }
        position += 12 + size;
    }
    return true;
}

/// A tag that is present and non-empty.
[[nodiscard]] bool flagSet(const MetadataMap& tags, std::string_view key) {
    return !tags.first(key).empty();
}

[[nodiscard]] int intTag(const MetadataMap& tags, std::string_view key, int fallback) {
    const std::string_view text = tags.first(key);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(std::string{text});
    } catch (...) {
        return fallback;
    }
}

class TwoSfDecoder final : public IDecoder {
public:
    ~TwoSfDecoder() override { TwoSfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    /// Tags only; the DS waits for the first frame anyone wants. Unlike GSF,
    /// nothing here needs the machine running to be knowable -- the SPU rate is
    /// fixed by the ARM7 clock -- so this takes the cheap path the USF core
    /// takes, and a library scan does not boot a DS per track.
    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf || psf->version != kTwoSfVersion) {
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

        format_.sampleRate    = kSampleRate;
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
        props.codec       = "2SF";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        if (framePos_ >= totalFrames_ || !start()) {
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
        out.streamTimestamp = static_cast<double>(framePos_) / kSampleRate;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);
        std::memcpy(dst, scratch_.data(), got * kChannels * sizeof(std::int16_t));
        out.setFrameCount(got);

        framePos_ += static_cast<std::int64_t>(got);
        return true;
    }

    /// Backwards means rebuilding the machine: melonDS has no rewind, and its
    /// reset would not replay the `_2sf_initial_frames` warm-up that start()
    /// does. Cog likewise tears the decoder down and re-initialises it.
    std::int64_t seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_ || !nds_) {
            nds_.reset();
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

    void close() override { nds_.reset(); }

private:
    /// Builds the cartridge and boots the DS, once, on the first frame wanted.
    [[nodiscard]] bool start() {
        if (nds_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        // Nested tags because `_2sf_initial_frames` belongs to the game and so
        // lives in the .2sflib rather than in each .mini2sf.
        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, kTwoSfVersion, /*wantNestedTags=*/true);
        if (!psf || psf->empty()) {
            return false;
        }

        Image rom;
        Image save;
        for (const codecs::PsfProgram& program : psf->programs) {
            if (program.exe.size() >= 8 &&
                !applyMap(rom, program.exe.data(), program.exe.size(),
                          /*roundSize=*/true)) {
                return false;
            }
            // Parsed and then dropped, exactly as Cog does. melonDS is given
            // only the cartridge; the save image has nowhere to go. Doing the
            // work anyway means a rip with a corrupt SAVE chunk is refused here
            // rather than playing something arbitrary, which is the behaviour
            // the format's own CRC is there to provide.
            if (!program.reserved.empty() && !loadSave(save, program.reserved)) {
                return false;
            }
        }
        if (rom.declared == 0) {
            return false;
        }

        melonDS::NDSArgs args;
        // No JIT: this build does not include one. The field is documented as
        // ignored in that case, and nullopt says so rather than relying on it.
        args.JIT              = std::nullopt;
        args.BitDepth         = melonDS::AudioBitDepth::Auto;
        args.Interpolation    = melonDS::AudioInterpolation::Cubic;
        args.OutputSampleRate = kSampleRate;
        args.GDB              = std::nullopt;

        auto nds = std::make_unique<melonDS::NDS>(std::move(args), nullptr);

        // ParseROM takes ownership, so the cartridge bytes are handed over
        // rather than borrowed -- unlike mGBA, which reads from ours.
        auto owned = std::make_unique<melonDS::u8[]>(rom.bytes.size());
        std::memcpy(owned.get(), rom.bytes.data(), rom.bytes.size());
        auto cart = melonDS::NDSCart::ParseROM(std::move(owned),
                                               static_cast<melonDS::u32>(rom.declared));
        if (!cart) {
            return false;
        }
        nds->SetNDSCart(std::move(cart));
        nds->SetGBACart(nullptr);

        nds->Reset();
        // Without this the firmware sits waiting for a charger and the game
        // never starts.
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
        if (nds->NeedsDirectBoot()) {
            nds->SetupDirectBoot("dummy.nds");
        }
        nds->Start();

        nds_ = std::move(nds);

        // `_2sf_initial_frames`: how long the game needs before its sound
        // driver is producing anything worth hearing. This is 2SF's answer to
        // the dead air a USF opens with, except the rip states it rather than
        // leaving it to be measured.
        const int initialFrames = intTag(psf->tags, "_2sf_initial_frames", -1);
        for (int frame = 0; frame < initialFrames; ++frame) {
            nds_->RunFrame();
            nds_->SPU.DrainOutput();
        }
        return true;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(
            std::llround(std::max(0.0, seconds) * kSampleRate));
    }

    /// Drains the SPU, running whole DS frames when it comes up short. A null
    /// buffer discards, which is what seeking wants.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        std::size_t filled = 0;
        int         spun   = 0;
        while (filled < frames) {
            int available = nds_->SPU.GetOutputSize();
            if (available == 0) {
                if (spun >= kMaxSpinFrames) {
                    break;
                }
                nds_->RunFrame();
                ++spun;
                continue;
            }
            spun = 0;

            const auto wanted = static_cast<int>(
                std::min<std::size_t>(static_cast<std::size_t>(available),
                                      frames - filled));
            if (out != nullptr) {
                filled += static_cast<std::size_t>(
                    nds_->SPU.ReadOutput(out + filled * kChannels, wanted));
            } else {
                nds_->SPU.DrainOutput();
                filled += static_cast<std::size_t>(wanted);
            }
        }
        return filled;
    }

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

    /// A DS frame is 1/60 s of emulated time. Sixty of them producing no audio
    /// at all is a rip that has hung, not one that is merely quiet.
    static constexpr int kMaxSpinFrames = 60;

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    Url                              url_;
    std::unique_ptr<melonDS::NDS>    nds_;
    AudioFormat                      format_{};
    std::int64_t                     framePos_    = 0;
    std::int64_t                     fadeStart_   = 0;
    std::int64_t                     totalFrames_ = 0;
    double                           volume_      = 1.0;
    MetadataMap                      tags_;

    std::vector<std::int16_t> scratch_;
};

constexpr std::string_view kExtensions[] = {"2sf", "mini2sf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_twosf(xpcog::PluginRegistry& r) {
    // Not `2sflib`, for the same reason `usflib` and `gsflib` are absent.
    r.addDecoder({
        .name       = "TwoSfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::TwoSfDecoder>();
        },
        .available = nullptr,
    });
}
