// Commodore 64 tunes, through libsidplayfp.
//
// Port of Cog Plugins/sidplay (SidDecoder, SidContainer, SidMetadataReader).
//
// A .sid is not audio and not even a sound-chip program in the way an NSF is.
// It is 6510 machine code with two entry points -- an init routine and a play
// routine -- and playing it means running a Commodore 64: the CPU, both CIAs,
// enough VIC-II to raise the raster interrupt the play routine is usually
// driven by, and the SID itself. libsidplayfp is that machine, and reSIDfp is
// its SID.
//
// Three things follow from that and shape everything below.
//
// A tune has no length. The play routine is called forever, so duration comes
// from settings the way it does for the PSF family. The High Voltage SID
// Collection ships a Songlengths database keyed by MD5 of the tune image, which
// is where real per-track times live; libsidplayfp can read one, and this does
// not, because the file is not something the player can assume exists.
//
// A tune usually has several. Subsongs are the norm rather than the exception
// on the C64 -- one file holds the title music, the in-game music and the
// jingles -- so this registers a container as well as a decoder, and addresses
// subsongs by URL fragment the way the GME codec does. Note that libsidplayfp
// numbers them from **one**, where GME numbers from zero.
//
// A tune is mono unless it is not. The C64 had one SID; a tune can ask for two
// or three, and players sum the extras into a stereo image. So the channel
// count is not a property of the format but of the individual file, and is only
// known after the tune is loaded.

#include "common/SourceBytes.hpp"
#include "common/TextEncoding.hpp"
#include "C64Roms.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidInfo.h>
// The reSIDfp builder's header sits in two places depending on where
// libsidplayfp came from. ports/libsidplayfp installs it flat, as
// <sidplayfp/residfp.h>, which is what Cog's own plugin includes and what
// upstream's autotools install produced for years; a distribution package keeps
// the source tree's layout and installs it under builders/. Both are the same
// header. See cmake/XPCogSystemDeps.cmake for when the system one is used --
// only for libsidplayfp 2.x, since 3.0 changed play() out from under this file.
#if defined(__has_include)
#  if __has_include(<sidplayfp/residfp.h>)
#    include <sidplayfp/residfp.h>
#  else
#    include <sidplayfp/builders/residfp.h>
#  endif
#else
#  include <sidplayfp/residfp.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::size_t kFramesPerRead = 1024;

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr double kDefaultSampleRate = 44100.0;
constexpr double kMinSampleRate     = 8000.0;
constexpr double kMaxSampleRate     = 192000.0;

/// Cog's ReSIDfpBuilder settings, unchanged. The two curve values pick where
/// between the extremes of chip-to-chip variation the filter sits: the 6581's
/// filter in particular varied enough between individual chips that there is no
/// single correct answer, and 0.5 is the middle.
constexpr double kFilter6581Curve = 0.5;
constexpr double kFilter8580Curve = 0.5;

/// A fixed power-on delay, which is a deliberate difference from Cog.
///
/// `SidConfig::DEFAULT_POWER_ON_DELAY` is deliberately one past
/// `MAX_POWER_ON_DELAY`, and libsidplayfp reads anything above the maximum as
/// "pick one at random" -- from a generator seeded with `time(0)`. That models
/// something real: a C64 does not come up in the same state twice, and a tune
/// that reads uninitialised memory sounds slightly different each time it is
/// switched on.
///
/// It is the wrong default for a music player. It means the same file decodes
/// to different samples on every run, which costs reproducible output for the
/// listener and makes the format untestable -- two subsongs of one tune differ
/// whether or not subsong selection works at all, so the test that would prove
/// it cannot tell the two apart. Found exactly that way: the subsong test
/// passed with `selectSong()` sabotaged.
///
/// The midpoint of the valid range, so it is a plausible machine rather than a
/// corner of one.
constexpr std::uint_least16_t kPowerOnDelay = SidConfig::MAX_POWER_ON_DELAY / 2;

/// The subsong a fragment names. libsidplayfp numbers songs from one and treats
/// zero as "the tune's own default", which is what an unfragmented URL wants.
[[nodiscard]] unsigned int songFromFragment(const Url& url) {
    const std::string_view fragment = url.fragment();
    unsigned int           value    = 0;
    for (const char c : fragment) {
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + static_cast<unsigned int>(c - '0');
    }
    return value;
}

/// Reads a tune's header without starting a machine.
///
/// Used by both the container's expand and the decoder's open, since neither
/// needs the emulator to answer "how many songs" or "what is it called".
[[nodiscard]] std::unique_ptr<SidTune> loadTune(ISource& source) {
    const auto data = codecs::readAllBytes(source);
    if (!data || data->empty()) {
        return nullptr;
    }
    auto tune = std::make_unique<SidTune>(
        reinterpret_cast<const std::uint_least8_t*>(data->data()),
        static_cast<std::uint_least32_t>(data->size()));
    if (!tune->getStatus()) {
        return nullptr;
    }
    return tune;
}

class SidDecoder final : public IDecoder {
public:
    ~SidDecoder() override { SidDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        std::unique_ptr<SidTune> tune = loadTune(*source);
        if (!tune) {
            return false;
        }

        sampleRate_ = kDefaultSampleRate;
        if (settings_ != nullptr) {
            const auto configured = static_cast<double>(settings_->SynthSampleRate());
            if (configured >= kMinSampleRate && configured <= kMaxSampleRate) {
                sampleRate_ = configured;
            }
        }

        tune->selectSong(songFromFragment(source->url()));

        const SidTuneInfo* info  = tune->getInfo();
        const int          chips = (info != nullptr) ? info->sidChips() : 1;

        auto engine = std::make_unique<sidplayfp>();

        // Without the ROMs a tune whose play routine calls into KERNAL simply
        // does not run. See C64Roms.hpp.
        engine->setRoms(kernel, basic, chargen);

        auto builder = std::make_unique<ReSIDfpBuilder>("ReSIDfp");
        builder->create(static_cast<unsigned int>(std::max(1, chips)));
        if (!builder->getStatus()) {
            return false;
        }
        builder->filter(true);
        builder->filter6581Curve(kFilter6581Curve);
        builder->filter8580Curve(kFilter8580Curve);

        SidConfig config    = engine->config();
        config.frequency    = static_cast<std::uint_least32_t>(std::ceil(sampleRate_));
        config.sidEmulation = builder.get();
        // A tune asking for a second or third SID is mixed to stereo; one SID
        // is mono, and widening it would be inventing a stereo image the C64
        // never produced.
        config.playback = (chips > 1) ? SidConfig::STEREO : SidConfig::MONO;
        config.powerOnDelay = kPowerOnDelay;

        // Configure *before* load, which is the reverse of Cog's order and is
        // the whole reason kPowerOnDelay takes effect at all.
        //
        // Player::config() calls initialise() and only then assigns `m_cfg =
        // cfg`, so a config() made while a tune is loaded initialises from the
        // *previous* settings and stores the new ones for next time. Configure
        // first, with no tune, and that block is skipped entirely; load() then
        // re-configures with `force` set -- "must re-configure on fly for
        // stereo support", says the comment there -- and by then m_cfg is
        // this one. Measured, not reasoned: with Cog's order the delay came
        // back as a fresh random number on every run.
        //
        // The chip count comes from the tune rather than from
        // `engine->info().maxsids()` for the same reason. maxsids is only
        // filled in once a tune is loaded, and nothing is loaded yet.
        if (!engine->config(config)) {
            return false;
        }
        if (!engine->load(tune.get())) {
            return false;
        }

        channels_ = (config.playback == SidConfig::STEREO) ? 2 : 1;

        readTags(info);

        double lengthSeconds = (settings_ != nullptr)
                                   ? settings_->SynthDefaultSeconds()
                                   : 150.0;
        double fadeSeconds   = (settings_ != nullptr)
                                   ? settings_->SynthDefaultFadeSeconds()
                                   : 8.0;
        if (lengthSeconds < 0.0) {
            lengthSeconds = 150.0;
        }
        if (fadeSeconds < 0.0) {
            fadeSeconds = 0.0;
        }

        fadeStart_   = toFrames(lengthSeconds);
        totalFrames_ = toFrames(lengthSeconds + fadeSeconds);

        format_.sampleRate    = sampleRate_;
        format_.channels      = channels_;
        format_.channelConfig = (channels_ == 2) ? 0x3 : 0x4;  // FL|FR, or FC
        format_.format        = SampleFormat::S16;
        format_.bitsPerSample = 16;

        // The engine holds pointers into both, so they outlive it in reverse.
        tune_     = std::move(tune);
        builder_  = std::move(builder);
        engine_   = std::move(engine);
        framePos_ = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "SID";
        props.encoding    = "synthesized";
        return props;
    }

    [[nodiscard]] MetadataMap metadata() const override { return tags_; }

    bool readAudio(AudioChunk& out) override {
        // Asked per read, not latched at open: the listener can switch
        // repeat-one on part-way through a tune and expects the fade to stop
        // coming. A SID has no length of its own -- the play routine is called
        // until something stops it -- so the length being overrun here is one
        // this player invented in the first place.
        const bool endless = loopForever(settings_);
        if (!engine_ || (!endless && framePos_ >= totalFrames_)) {
            return false;
        }

        const auto want =
            endless ? static_cast<std::size_t>(kFramesPerRead)
                    : static_cast<std::size_t>(std::min<std::int64_t>(
                          static_cast<std::int64_t>(kFramesPerRead),
                          totalFrames_ - framePos_));
        scratch_.resize(want * channels_);

        const std::size_t got = render(scratch_.data(), want);
        if (got == 0) {
            return false;
        }

        applyGain(scratch_.data(), got);

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / sampleRate_;
        out.streamTimeRatio = 1.0;

        std::byte* dst = out.allocFrames(got);
        std::memcpy(dst, scratch_.data(), got * channels_ * sizeof(std::int16_t));
        out.setFrameCount(got);

        framePos_ += static_cast<std::int64_t>(got);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);
        if (!engine_ || !tune_) {
            return -1;
        }

        // There is no stream to seek in: a C64 has state, and the only way back
        // to an earlier point is to reset the machine and run it again.
        if (frame < framePos_) {
            engine_->stop();
            if (!engine_->load(tune_.get())) {
                return -1;
            }
            framePos_ = 0;
        }

        while (framePos_ < frame) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       frame - framePos_));
            scratch_.resize(step * channels_);
            const std::size_t got = render(scratch_.data(), step);
            if (got == 0) {
                return -1;
            }
            framePos_ += static_cast<std::int64_t>(got);
        }
        return framePos_;
    }

    void close() override {
        // The engine points into the builder and the tune, so it goes first.
        engine_.reset();
        builder_.reset();
        tune_.reset();
    }

private:
    void readTags(const SidTuneInfo* info) {
        tags_.clear();
        if (info == nullptr) {
            return;
        }
        // PSID's header carries exactly three 32-byte fields, in this order,
        // and libsidplayfp exposes them as an indexed list rather than by name.
        static constexpr std::string_view kKeys[] = {"title", "artist", "album"};
        const unsigned int count =
            std::min<unsigned int>(info->numberOfInfoStrings(), 3);
        for (unsigned int i = 0; i < count; ++i) {
            const char* text = info->infoString(i);
            if (text != nullptr && *text != '\0') {
                // The header fields are ISO-8859-1 by the format's own
                // definition, not UTF-8, and Scandinavian demo-scene handles
                // are common enough that guessing wrong is visible.
                tags_.set(kKeys[i], codecs::latin1ToUtf8(text));
            }
        }
    }

    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(
            std::ceil(std::max(0.0, seconds) * sampleRate_));
    }

    /// sidplayfp::play() counts *samples*, not frames, and returns how many it
    /// produced. It fills the whole buffer unless the engine has stopped.
    [[nodiscard]] std::size_t render(std::int16_t* out, std::size_t frames) {
        const std::uint_least32_t samples =
            static_cast<std::uint_least32_t>(frames * channels_);
        const std::uint_least32_t produced = engine_->play(out, samples);
        if (produced == 0 || !engine_->isPlaying()) {
            return produced / channels_;
        }
        return produced / channels_;
    }

    void applyGain(std::int16_t* frames, std::size_t count) {
        // No fade while looping for ever: the fade is what turns a tune that
        // never ends into a track that does.
        const std::int64_t fadeLength =
            loopForever(settings_) ? 0 : totalFrames_ - fadeStart_;
        if (fadeLength <= 0 ||
            framePos_ + static_cast<std::int64_t>(count) <= fadeStart_) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            if (position <= fadeStart_) {
                continue;
            }
            const double gain = static_cast<double>(totalFrames_ - position) /
                                static_cast<double>(fadeLength);
            for (std::uint32_t channel = 0; channel < channels_; ++channel) {
                const double scaled =
                    static_cast<double>(frames[i * channels_ + channel]) * gain;
                frames[i * channels_ + channel] =
                    static_cast<std::int16_t>(std::clamp(scaled, -32768.0, 32767.0));
            }
        }
    }

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    std::unique_ptr<SidTune>        tune_;
    std::unique_ptr<ReSIDfpBuilder> builder_;
    std::unique_ptr<sidplayfp>      engine_;

    AudioFormat  format_{};
    double       sampleRate_  = kDefaultSampleRate;
    std::uint32_t channels_   = 1;
    std::int64_t framePos_    = 0;
    std::int64_t fadeStart_   = 0;
    std::int64_t totalFrames_ = 0;
    MetadataMap  tags_;

    std::vector<std::int16_t> scratch_;
};

/// A tune with subsongs expands to one URL per song, numbered from one.
std::vector<Url> expandTune(const Url& url, ISource& source,
                            const PluginRegistry& /*registry*/) {
    if (!url.fragment().empty()) {
        return {url};
    }

    const std::unique_ptr<SidTune> tune = loadTune(source);
    if (!tune) {
        return {url};
    }
    const SidTuneInfo* info = tune->getInfo();
    if (info == nullptr || info->songs() <= 1) {
        return {url};
    }

    std::vector<Url> songs;
    songs.reserve(info->songs());
    for (unsigned int i = 1; i <= info->songs(); ++i) {
        songs.push_back(url.withFragment(std::to_string(i)));
    }
    return songs;
}

constexpr std::string_view kExtensions[] = {"sid", "mus"};

}  // namespace
}  // namespace xpcog

void xpcog_register_sid(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "SidContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .expand     = &xpcog::expandTune,
    });

    r.addDecoder({
        .name       = "SidDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::SidDecoder>();
        },
        .available = nullptr,
    });
}
