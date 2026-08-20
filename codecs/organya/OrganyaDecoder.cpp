// Organya (.org) -- Cave Story's music, played by the synthesiser in
// OrganyaSynth.cpp.
//
// Port of Cog Plugins/Organya. The player is the same one; this file is the
// part Cog wrote around it, and it differs in three places:
//
//   * **The length agrees with the audio.** The format counts time in whole
//     milliseconds per beat, and the mixer counts it in whole samples per beat
//     -- a conversion that truncates. Cog computes the track length from the
//     milliseconds (`ms_per_beat * 1e-3 * beats * rate`) while rendering from
//     the samples, so on any song whose beat is not a whole number of samples
//     the two disagree and the gap grows with every beat. A 125 ms beat at
//     44,100 Hz loses half a sample each time, which over a four-minute song is
//     a track that claims about a thousand frames it never produces. Here the
//     length is beats times samples-per-beat, which is what actually comes out.
//
//   * **Seeking keeps the notes that are still sounding.** Cog moves `cur_beat`
//     straight to the target, so every note held across the seek point vanishes
//     and every instrument keeps whatever volume and panning it had before --
//     the first seconds after a seek are a different arrangement of the song.
//     This walks the beats instead, with the mixing switched off; see
//     Song::renderBeat().
//
//   * **No `samplesDiscard`.** Cog seeks to a beat and then asks the next chunk
//     to drop the first N samples of it, which loses those samples' worth of
//     the notes that start on that beat. Here a beat is rendered whole and the
//     position within it is just an offset into the buffer.
//
// Organya carries no metadata at all -- no title, no author, not even a comment
// field -- so there is nothing to read and metadata() says so.

#include "OrganyaSynth.hpp"

#include "common/SourceBytes.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

constexpr std::string_view kExtensionList[] = {"org"};
constexpr std::span<const std::string_view> kExtensions{kExtensionList};

constexpr std::uint32_t kChannels = 2;

/// What Cog falls back to when synthSampleRate is unset or out of range, and
/// the clamp it applies.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

/// Cog's clamp on synthDefaultLoopCount, and the same one the other synth
/// decoders here use.
constexpr int kMaxLoopCount = 10;

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

class OrganyaDecoder final : public IDecoder {
public:
    ~OrganyaDecoder() override { OrganyaDecoder::close(); }

    void setSettings(const Settings* settings) override { settings_ = settings; }

    bool open(ISource* source) override {
        close();
        if (source == nullptr) {
            return false;
        }

        const auto data = codecs::readAllBytes(*source);
        if (!data) {
            return false;
        }

        rate_ = preferredRate(settings_);
        song_ = std::make_unique<organya::Song>();
        if (!song_->load(*data, static_cast<double>(rate_))) {
            song_.reset();
            return false;
        }

        samplesPerBeat_ = static_cast<std::int64_t>(song_->samplesPerBeat());
        bodyFrames_     = bodyFrameCount();
        fadeFrames_     = fadeFrameCount();
        totalFrames_    = bodyFrames_ + fadeFrames_;

        format_.sampleRate    = static_cast<double>(rate_);
        format_.channels      = kChannels;
        format_.channelConfig = 0x3;  // FL | FR
        format_.format        = SampleFormat::F32;
        format_.bitsPerSample = 32;

        pending_.assign(static_cast<std::size_t>(samplesPerBeat_) * kChannels, 0.0F);
        pendingValid_  = false;
        beatsAdvanced_ = 0;
        framePos_      = 0;
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "Organya";
        props.encoding    = "synthesized";
        return props;
    }

    /// Nothing to report: the format has no metadata fields. Deliberately empty
    /// rather than a title guessed from the filename -- the library builds that
    /// fallback itself, for every codec, and doing it here would only make this
    /// one inconsistent with the rest.
    [[nodiscard]] MetadataMap metadata() const override { return {}; }

    bool readAudio(AudioChunk& out) override {
        // Asked per read rather than latched at open: the listener can switch
        // repeat-one on part-way through a song and expects the fade to stop
        // coming.
        const bool endless = loopForever(settings_);
        if (!song_ || (!endless && framePos_ >= totalFrames_)) {
            return false;
        }

        const auto want = static_cast<std::size_t>(
            endless ? kFramesPerRead
                    : std::min<std::int64_t>(kFramesPerRead, totalFrames_ - framePos_));
        if (want == 0) {
            return false;
        }

        out.clear();
        out.setFormat(format_);
        out.lossless        = false;
        out.streamTimestamp = static_cast<double>(framePos_) / static_cast<double>(rate_);
        out.streamTimeRatio = 1.0;

        auto*             dst    = reinterpret_cast<float*>(out.allocFrames(want));
        const std::size_t filled = render(dst, want);
        if (filled == 0) {
            return false;
        }
        applyFade(dst, filled, endless);
        out.setFrameCount(filled);

        framePos_ += static_cast<std::int64_t>(filled);
        return true;
    }

    std::int64_t seek(std::int64_t frame) override {
        if (!song_) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        const std::int64_t targetBeat = frame / samplesPerBeat_;

        // Already holding that beat: nothing to replay, just move within it.
        if (pendingValid_ && beatsAdvanced_ - 1 == targetBeat) {
            framePos_ = frame;
            return framePos_;
        }

        // The song can only go forwards, so anything behind where it stands
        // means starting over. Going forwards costs one skipped beat per beat,
        // which for the longest song in the format is a few thousand iterations
        // of arithmetic and no mixing at all.
        if (targetBeat < beatsAdvanced_) {
            song_->reset();
            beatsAdvanced_ = 0;
        }
        while (beatsAdvanced_ < targetBeat) {
            song_->renderBeat(nullptr);
            ++beatsAdvanced_;
        }

        pendingValid_ = false;
        framePos_     = frame;
        return framePos_;
    }

    void close() override {
        song_.reset();
        pending_.clear();
        pendingValid_  = false;
        beatsAdvanced_ = 0;
        framePos_      = 0;
    }

private:
    static constexpr std::int64_t kFramesPerRead = 1024;

    /// How long the song is, in frames, before the fade.
    ///
    /// The intro once, then the loop as many times as the listener asked for.
    /// That is the whole of it: a beat is a fixed number of frames, and the
    /// sequencer visits beats 0..loopEnd-1 and then loopStart..loopEnd-1 for
    /// ever, so the count is exact rather than estimated.
    [[nodiscard]] std::int64_t bodyFrameCount() const {
        const int loops =
            (settings_ != nullptr) ? settings_->SynthDefaultLoopCount() : 2;
        // At least one pass: zero is a song of no length, which is not what
        // anybody means by "do not loop".
        const auto passes = static_cast<std::int64_t>(std::clamp(loops, 1, kMaxLoopCount));

        const auto start = static_cast<std::int64_t>(song_->loopStart());
        const auto end   = static_cast<std::int64_t>(song_->loopEnd());
        if (end <= start) {
            // No loop to count, so there is no length to derive -- the song
            // simply runs on past its last event. Every .org in the wild loops,
            // but the header is free not to, and falling back on the general
            // "how long is a synthesised track with no length" setting is
            // better than a song of zero frames.
            const double seconds =
                (settings_ != nullptr) ? settings_->SynthDefaultSeconds() : 150.0;
            return static_cast<std::int64_t>(std::max(0.0, seconds) *
                                             static_cast<double>(rate_));
        }
        return (start + ((end - start) * passes)) * samplesPerBeat_;
    }

    [[nodiscard]] std::int64_t fadeFrameCount() const {
        const double seconds =
            (settings_ != nullptr) ? settings_->SynthDefaultFadeSeconds() : 8.0;
        return static_cast<std::int64_t>(
            std::ceil(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    /// Fills `out` with `frames` frames, rendering whole beats and carrying the
    /// remainder of the last one across calls.
    ///
    /// Nothing asks for a multiple of a beat -- a beat at 170 ms is 7,497
    /// frames -- so without somewhere to keep the tail every read would either
    /// drop it or render it twice.
    [[nodiscard]] std::size_t render(float* out, std::size_t frames) {
        std::size_t filled = 0;
        while (filled < frames) {
            const std::int64_t beat = (framePos_ + static_cast<std::int64_t>(filled)) /
                                      samplesPerBeat_;
            if (!pendingValid_ || beatsAdvanced_ - 1 != beat) {
                song_->renderBeat(pending_.data());
                ++beatsAdvanced_;
                pendingValid_ = true;
            }

            const auto offset = static_cast<std::size_t>(
                (framePos_ + static_cast<std::int64_t>(filled)) % samplesPerBeat_);
            const std::size_t take =
                std::min(frames - filled,
                         static_cast<std::size_t>(samplesPerBeat_) - offset);

            std::memcpy(out + (filled * kChannels), pending_.data() + (offset * kChannels),
                        take * kChannels * sizeof(float));
            filled += take;
        }
        return filled;
    }

    /// Ramps the tail to silence, which is what turns a song that never ends
    /// into a track with a length. Not applied while looping for ever: that is
    /// the listener saying they did not want the song to end.
    void applyFade(float* frames, std::size_t count, bool endless) const {
        if (endless || fadeFrames_ <= 0 ||
            framePos_ + static_cast<std::int64_t>(count) <= bodyFrames_) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t position = framePos_ + static_cast<std::int64_t>(i);
            if (position <= bodyFrames_) {
                continue;
            }
            const auto gain = static_cast<float>(
                std::max(0.0, static_cast<double>(totalFrames_ - position) /
                                  static_cast<double>(fadeFrames_)));
            for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
                frames[(i * kChannels) + channel] *= gain;
            }
        }
    }

    const Settings* settings_ = nullptr;

    std::unique_ptr<organya::Song> song_;
    AudioFormat                    format_{};

    int          rate_           = kDefaultRate;
    std::int64_t samplesPerBeat_ = 0;
    std::int64_t bodyFrames_     = 0;
    std::int64_t fadeFrames_     = 0;
    std::int64_t totalFrames_    = 0;
    std::int64_t framePos_       = 0;

    /// One rendered beat, and how many beats the song has been advanced
    /// through. `pending_` holds beat `beatsAdvanced_ - 1` when `pendingValid_`
    /// -- which a seek clears, because skipping a beat leaves the buffer
    /// holding whatever it held before.
    std::vector<float> pending_;
    std::int64_t       beatsAdvanced_ = 0;
    bool               pendingValid_  = false;
};

}  // namespace
}  // namespace xpcog

void xpcog_register_organya(xpcog::PluginRegistry& r) {
    r.addDecoder({
        .name       = "OrganyaDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::OrganyaDecoder>();
        },
        .available = nullptr,
    });
}
