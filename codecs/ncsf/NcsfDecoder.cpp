// Nintendo DS sequenced rips, through SSEQPlayer. The sixth of HighlyComplete's
// cores, and the only one that is not an emulator.
//
// Port of the `type == 0x25` paths of Cog Plugins/HighlyComplete/HCDecoder.mm.
//
// An NCSF is not a program. Its `exe` section holds an SDAT -- the DS's standard
// sound archive -- and its `reserved` section holds nothing but a four-byte
// sequence number saying which SSEQ inside that archive to play. SSEQPlayer
// parses the sequence, resolves the SBNK instrument bank and the SWAR/SWAV
// samples it names, and mixes the DS's sixteen channels in software. No ARM
// code is executed at any point.
//
// So this shares the container with its five siblings and nothing else. There
// is no save state to upload, no ROM to assemble at the right addresses, no
// CPU-APU handshake to keep honest, and none of the section-layout care the
// others need: the whole archive arrives in one piece.
//
// The two things it does need that the others do not: it is C++ that reports a
// malformed archive by *throwing*, and the same 2SF trick of a shared library
// applies -- one `.ncsflib` holds the SDAT and each `.minincsf` is a few hundred
// bytes naming a sequence number within it.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <SSEQPlayer/Player.h>
#include <SSEQPlayer/SDAT.h>
#include <SSEQPlayer/common.h>
#include <SSEQPlayer/consts.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// The PSF version byte for NCSF. Verified against Cog's HCDecoder.mm.
constexpr std::uint8_t kNcsfVersion = 0x25;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// SSEQPlayer synthesises at whatever rate it is told, so unlike the emulator
/// cores this is a choice rather than a property of hardware. 44100 is Cog's.
constexpr int kSampleRate = 44100;

[[nodiscard]] std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// The SDAT and the sequence number to play out of it, assembled from the chain.
struct Archive {
    std::vector<std::uint8_t> sdat;
    std::uint32_t             sequence = 0;
};

/// One section of the chain.
///
/// This is the simplest loader of the six, and worth saying why: `exe` is the
/// SDAT itself, whose own header states its length at offset 8, and later
/// sections replace rather than overlay. The `.minincsf` contributes only the
/// sequence number in `reserved`, so the archive comes from the library and the
/// track selection from the file.
[[nodiscard]] bool applySection(Archive& archive, const codecs::PsfProgram& program) {
    if (program.reserved.size() >= 4) {
        archive.sequence = readLe32(program.reserved.data());
    }

    if (program.exe.size() >= 12) {
        const std::uint32_t stated = readLe32(program.exe.data() + 8);
        if (stated > program.exe.size()) {
            return false;  // states more than it carries
        }
        if (archive.sdat.size() < stated) {
            archive.sdat.resize(stated, 0);
        }
        std::memcpy(archive.sdat.data(), program.exe.data(), stated);
    }
    return true;
}

class NcsfDecoder final : public IDecoder {
public:
    ~NcsfDecoder() override { NcsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf || psf->version != kNcsfVersion) {
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
        props.codec       = "NCSF";
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
        if (!render(want)) {
            return false;
        }

        auto* frames = reinterpret_cast<std::int16_t*>(mix_.data());
        applyGain(frames, want);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp =
            static_cast<double>(framePos_) / static_cast<double>(kSampleRate);
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(want);
        std::memcpy(dst, mix_.data(), want * kChannels * sizeof(std::int16_t));
        out.setFrameCount(want);

        framePos_ += static_cast<std::int64_t>(want);
        return true;
    }

    /// A sequence player has no rewind either: seeking backwards re-parses the
    /// archive and replays the sequence from its start. Forwards renders on and
    /// discards, which is cheap here compared with the emulator cores.
    std::int64_t seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        if (frame < framePos_ || !player_) {
            player_.reset();
            sdat_.reset();
            framePos_ = 0;
        }
        if (!start()) {
            return -1;
        }

        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       frame - framePos_));
            if (!render(step)) {
                return -1;
            }
            framePos_ += static_cast<std::int64_t>(step);
        }
        return framePos_;
    }

    void close() override {
        // Destroyed before the SDAT: the player holds pointers into the
        // sequence and instrument bank the archive owns.
        player_.reset();
        sdat_.reset();
    }

private:
    /// Parses the archive and prepares the player, once.
    ///
    /// Everything here can throw: SSEQPlayer reports a malformed SDAT, a
    /// sequence number that is not in it, or a bank it cannot resolve by
    /// throwing rather than by returning a status. Cog wraps the same calls in
    /// try/catch, and a rip is an untrusted file, so an exception is a decoder
    /// that declines rather than a process that dies.
    [[nodiscard]] bool start() {
        if (player_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, kNcsfVersion);
        if (!psf || psf->empty()) {
            return false;
        }

        Archive archive;
        for (const codecs::PsfProgram& program : psf->programs) {
            if (!applySection(archive, program)) {
                return false;
            }
        }
        if (archive.sdat.empty()) {
            return false;
        }

        try {
            PseudoFile file;
            file.data = &archive.sdat;
            file.pos  = 0;

            auto sdat = std::make_unique<SDAT>(file, archive.sequence);
            if (!sdat->sseq) {
                return false;
            }

            auto player           = std::make_unique<Player>();
            player->sampleRate    = kSampleRate;
            player->interpolation = INTERPOLATION_SINC;
            if (!player->Setup(sdat->sseq.get())) {
                return false;
            }
            player->Timer();

            // The archive has to outlive the player, which holds pointers into
            // it; `sdat_` keeps the parsed form and `sdatBytes_` the buffer that
            // form points into.
            sdatBytes_ = std::move(archive.sdat);
            sdat_      = std::move(sdat);
            player_    = std::move(player);
        } catch (const std::exception&) {
            player_.reset();
            sdat_.reset();
            return false;
        }

        mix_.assign(kFramesPerRead * kChannels * sizeof(std::int16_t), 0);
        return true;
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) *
                                                      static_cast<double>(kSampleRate)));
    }

    /// GenerateSamples always produces exactly what is asked for -- there is no
    /// hardware to wait on -- so unlike the emulator cores this cannot come up
    /// short, only throw.
    [[nodiscard]] bool render(std::size_t frames) {
        if (mix_.size() < frames * kChannels * sizeof(std::int16_t)) {
            mix_.assign(frames * kChannels * sizeof(std::int16_t), 0);
        }
        try {
            player_->GenerateSamples(mix_, 0, static_cast<unsigned>(frames));
        } catch (const std::exception&) {
            return false;
        }
        return true;
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
    std::vector<std::uint8_t> sdatBytes_;
    std::unique_ptr<SDAT>     sdat_;
    std::unique_ptr<Player>   player_;
    AudioFormat               format_{};
    std::int64_t              framePos_    = 0;
    std::int64_t              fadeStart_   = 0;
    std::int64_t              totalFrames_ = 0;
    double                    volume_      = 1.0;
    MetadataMap               tags_;

    std::vector<std::uint8_t> mix_;
};

constexpr std::string_view kExtensions[] = {"ncsf", "minincsf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_ncsf(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "NcsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::NcsfDecoder>();
        },
        .available = nullptr,
    });
}
