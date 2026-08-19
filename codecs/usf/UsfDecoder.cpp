// Nintendo 64 rips, through lazyusf2. The first of HighlyComplete's eight cores.
//
// Port of the `type == 0x21` paths of Cog Plugins/HighlyComplete/HCDecoder.mm.
// A USF is not audio and not even a music program: it is a Project 64 *save
// state*, captured at the moment the game's sound driver was running, plus the
// ROM words that driver reads. Playing one means booting an N64 into that state
// and recording what the Audio Interface sends out. So there is no bitrate, no
// source format, and -- the part that shapes everything below -- no ending.
//
// Three things follow from that, and each is a place this could quietly be
// wrong rather than visibly broken:
//
//   * The program is in `reserved`, not `exe`. Every other PSF variant this
//     tree will grow puts its executable in the `exe` section; USF leaves that
//     section empty and keeps the save state in `reserved`. A loader written
//     from the PSF spec alone reads the wrong half and gets silence.
//
//   * `length` is the only thing that ends the track. Without the tag the
//     emulator runs for ever, so the fallback below is not a nicety -- a track
//     with no duration never advances the playlist.
//
//   * A USF starts silent. The save state is taken slightly before the music
//     does anything, and how much slack varies by rip: often a fraction of a
//     second, sometimes several. Left in, every track of a set opens with dead
//     air of a different length and `length` measures from the wrong instant.
//     Cog trims it and so does this; see below.
//
// The `.miniusf` / `.usflib` split is the container's business, not this file's
// -- codecs/psf resolves the chain and hands over the sections in the order
// they must be applied.

#include "psf/PsfFile.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

extern "C" {
#include <lazyusf2/usf/usf.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {
namespace {

/// The PSF version byte for USF. Verified against Cog's HCDecoder.mm.
constexpr std::uint8_t kUsfVersion = 0x21;

constexpr std::uint32_t kChannels      = 2;
constexpr std::size_t   kFramesPerRead = 1024;

/// Shared with the GME decoder, and for the same reason: these are Cog's
/// synthSampleRate clamp.
constexpr int kDefaultRate = 44100;
constexpr int kMinRate     = 8000;
constexpr int kMaxRate     = 192000;

/// How far in to look for the first sound, and what counts as silence.
///
/// Both are Cog's: `silenceSeconds = 10` for USF, and a threshold of 8 on a
/// 16-bit sample. The threshold is a *delta* from the previous sample rather
/// than a distance from zero, which matters -- an emulated DAC sitting idle
/// often rests on a small non-zero DC level, and a test against zero would find
/// no silence at all in a track that is plainly silent.
constexpr std::int64_t kSilenceScanSeconds = 10;
constexpr int          kSilenceThreshold   = 8;

[[nodiscard]] int preferredRate(const Settings* settings) {
    if (settings == nullptr) {
        return kDefaultRate;
    }
    const int rate = settings->SynthSampleRate();
    return (rate < kMinRate || rate > kMaxRate) ? kDefaultRate : rate;
}

/// A tag that is present and non-empty. lazyusf2's two accuracy switches are
/// spelled this way in the rips -- `_enablecompare=1`, but the value is never
/// examined, only its presence. Cog does the same.
[[nodiscard]] bool flagSet(const MetadataMap& tags, std::string_view key) {
    return !tags.first(key).empty();
}

/// lazyusf2 allocates nothing until the first render and frees it in
/// usf_shutdown(), so the buffer and the emulator have to be released in that
/// order and never one without the other.
struct UsfState {
    explicit UsfState(std::size_t bytes) : storage(new std::byte[bytes]) {}
    ~UsfState() {
        if (storage) {
            usf_shutdown(storage.get());
        }
    }
    UsfState(const UsfState&)            = delete;
    UsfState& operator=(const UsfState&) = delete;

    [[nodiscard]] void* get() const { return storage.get(); }

    // usf_clear() aligns the emulator structure to a 4 KB boundary inside the
    // 8 KB of slack usf_get_state_size() adds, so plain new[] is enough.
    std::unique_ptr<std::byte[]> storage;
};

class UsfDecoder final : public IDecoder {
public:
    ~UsfDecoder() override { UsfDecoder::close(); }

    void setRegistry(const PluginRegistry* registry) override { registry_ = registry; }
    void setSettings(const Settings* settings) override { settings_ = settings; }

    /// Reads the tag block and stops there. **No emulator is started here** --
    /// that waits for the first readAudio() or seek(), which is what Cog does
    /// too: HCDecoder's -open: runs the metadata psf_load and nothing else, and
    /// -initializeDecoder is reached from -readAudio and -seek.
    ///
    /// It matters because of how a library scan works. Scanner::readMetadata
    /// opens a decoder for every file it walks, purely to ask for properties,
    /// and everything properties() reports comes from the tag block -- the
    /// `length` tag is the duration and the rest is fixed. Booting an N64 to
    /// answer a question the first few hundred bytes already answer would mean
    /// allocating the machine, walking the `_lib` chain and inflating a
    /// multi-megabyte .usflib once per track: 694 times over for the corpus
    /// this was written against, and eight times that once the rest of
    /// HighlyComplete's cores sit behind the same container.
    ///
    /// The cost is that a mini-PSF orphaned from its library now opens and then
    /// fails at playback, rather than failing to open -- the tags-only path
    /// stops before psflib follows `_lib` at all. Cog has the same behaviour,
    /// and the alternative is paying the full inflate on every scanned file to
    /// find out.
    ///
    /// Takes the URL rather than the bytes. psflib follows `_lib` by name, so
    /// the chain has to be walked through the registry from the top; `source`
    /// is already open on the outermost file and is simply not the way in.
    bool open(ISource* source) override {
        close();
        if (source == nullptr || registry_ == nullptr) {
            return false;
        }

        const std::optional<codecs::PsfFile> psf =
            codecs::readPsfTags(source->url(), *registry_);
        if (!psf) {
            return false;
        }

        // readPsfTags() probes rather than enforcing, so the version byte is
        // checked here instead. Feeding a GBA image to an N64 is not a near
        // miss -- it is arbitrary bytes at the reset vector.
        if (psf->version != kUsfVersion) {
            return false;
        }

        url_ = source->url();

        rate_   = preferredRate(settings_);
        tags_   = psf->tags;
        volume_ = (psf->volume > 0.0) ? psf->volume : 1.0;

        // Cog's chain: the rip's own length, else synthDefaultSeconds. The fade
        // travels with it -- a rip that states a length states its own fade or
        // wants none, and only the fallback reaches for the default fade.
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

    /// Boots the machine, on the first frame anyone actually wants. Everything
    /// deferred from open() happens here, once.
    [[nodiscard]] bool start() {
        if (state_) {
            return true;
        }
        if (registry_ == nullptr) {
            return false;
        }

        // Nested tags: `_enablecompare` and `_enablefifofull` belong to the
        // *game*, so a set puts them in the .usflib rather than repeating them
        // in each of its hundred .miniusf files. Reading only the outermost
        // file loses them, and the two of them are the difference between a rip
        // that renders correctly and one that renders subtly wrong.
        const std::optional<codecs::PsfFile> psf =
            codecs::loadPsf(url_, *registry_, kUsfVersion, /*wantNestedTags=*/true);
        if (!psf || psf->empty()) {
            return false;
        }

        auto state = std::make_unique<UsfState>(usf_get_state_size());
        usf_clear(state->get());

        // HLE audio: the sound microcode is recognised and run natively instead
        // of being emulated instruction by instruction on the RSP. Far faster,
        // and Cog turns it on unconditionally for USF.
        usf_set_hle_audio(state->get(), 1);

        for (const codecs::PsfProgram& program : psf->programs) {
            // A USF with anything in `exe` is not a USF. Cog's loader refuses
            // it outright rather than guessing, because the only way to produce
            // one is to have mislabelled some other format's file.
            if (!program.exe.empty()) {
                return false;
            }
            if (program.reserved.empty()) {
                continue;
            }
            if (usf_upload_section(state->get(), program.reserved.data(),
                                   program.reserved.size()) < 0) {
                return false;
            }
        }

        // After the upload, never before: usf_clear() zeroed both flags and the
        // tags that set them arrive while the chain is being walked.
        usf_set_compare(state->get(), flagSet(psf->tags, "_enablecompare") ? 1 : 0);
        usf_set_fifo_full(state->get(), flagSet(psf->tags, "_enablefifofull") ? 1 : 0);

        state_ = std::move(state);
        skipLeadingSilence();
        return true;
    }

    [[nodiscard]] TrackProperties properties() const override {
        TrackProperties props;
        props.format      = format_;
        props.totalFrames = totalFrames_;
        props.seekable    = true;
        props.lossless    = false;
        props.codec       = "USF";
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

        std::size_t got = takePending(want);
        if (got == 0) {
            scratch_.resize(want * kChannels);
            if (!render(scratch_.data(), want)) {
                return false;
            }
            got = want;
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

    /// There is no rewinding an emulator: seeking means restarting the machine
    /// and running it forward to the target and throwing the audio away.
    /// Backwards and forwards cost the same, and both cost real time.
    ///
    /// The discarded audio is still rendered into a buffer, even though
    /// usf_render_resampled() accepts a null pointer to mean "render and drop".
    /// That shortcut is not sample-exact: it converts the frame count back into
    /// the emulator's own rate with integer arithmetic and skips that many
    /// there instead, so the position it reaches drifts from the one playing
    /// through would have reached. Seeking to a beat and hearing a different
    /// one is not a rounding error to a listener.
    std::int64_t seek(std::int64_t frame) override {
        const bool wasRunning = state_ != nullptr;
        if (!start()) {
            return -1;
        }
        frame = std::clamp<std::int64_t>(frame, 0, totalFrames_);

        // start() has just booted a machine sitting at frame zero with its
        // leading silence already trimmed, so restarting it would only redo
        // both. Seeking on an already-running one has to, since there is no
        // rewinding an emulator.
        if (wasRunning) {
            usf_restart(state_->get());
            pending_.clear();
            pendingPos_ = 0;
            skipLeadingSilence();
        }
        framePos_ = 0;

        std::int64_t remaining = frame;
        while (remaining > 0) {
            const auto step = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(kFramesPerRead),
                                       remaining));
            // takePending() may hand back fewer frames than asked for -- the
            // silence scan leaves a partial block behind -- so the count that
            // comes back is the one that counts, not the one requested.
            std::size_t got = takePending(step);
            if (got == 0) {
                scratch_.resize(step * kChannels);
                if (!render(scratch_.data(), step)) {
                    return -1;
                }
                got = step;
            }
            remaining -= static_cast<std::int64_t>(got);
        }

        framePos_ = frame;
        return frame;
    }

    void close() override {
        state_.reset();
        pending_.clear();
        pendingPos_ = 0;
    }


private:
    [[nodiscard]] std::int64_t toFrames(double seconds) const {
        return static_cast<std::int64_t>(
            std::llround(std::max(0.0, seconds) * static_cast<double>(rate_)));
    }

    [[nodiscard]] bool render(std::int16_t* buffer, std::size_t frames) {
        return usf_render_resampled(state_->get(), buffer, frames, rate_) == nullptr;
    }

    /// Moves up to `want` frames out of the buffer left over from the silence
    /// scan and into `scratch_`. Returns how many, zero once it is drained.
    [[nodiscard]] std::size_t takePending(std::size_t want) {
        const std::size_t have = (pending_.size() / kChannels) - pendingPos_;
        if (have == 0) {
            pending_.clear();
            pendingPos_ = 0;
            return 0;
        }
        const std::size_t got = std::min(want, have);
        scratch_.assign(pending_.begin() + static_cast<std::ptrdiff_t>(pendingPos_ * kChannels),
                        pending_.begin() +
                            static_cast<std::ptrdiff_t>((pendingPos_ + got) * kChannels));
        pendingPos_ += got;
        return got;
    }

    /// Drops the dead air a USF opens with, so that frame zero is the first
    /// sound and `length` measures from there.
    ///
    /// The scan stops at the first frame that moves by more than the threshold,
    /// and whatever of that block came after it is kept -- rendering is not
    /// repeatable without restarting the machine, so audio read here cannot be
    /// read again. Ten seconds is the ceiling: past that the track is taken to
    /// be silent on purpose and left alone.
    void skipLeadingSilence() {
        const std::int64_t limit = kSilenceScanSeconds * rate_;
        std::int16_t       last[kChannels]{0, 0};

        std::vector<std::int16_t> block(kFramesPerRead * kChannels);
        for (std::int64_t scanned = 0; scanned < limit;
             scanned += static_cast<std::int64_t>(kFramesPerRead)) {
            if (!render(block.data(), kFramesPerRead)) {
                return;
            }
            for (std::size_t i = 0; i < kFramesPerRead; ++i) {
                const int left  = block[i * kChannels] - last[0];
                const int right = block[i * kChannels + 1] - last[1];
                if (std::abs(left) > kSilenceThreshold ||
                    std::abs(right) > kSilenceThreshold) {
                    pending_.assign(block.begin() +
                                        static_cast<std::ptrdiff_t>(i * kChannels),
                                    block.end());
                    pendingPos_ = 0;
                    return;
                }
                last[0] = block[i * kChannels];
                last[1] = block[i * kChannels + 1];
            }
        }
    }

    /// The fade and the `volume` tag, in one pass.
    ///
    /// The fade is what turns "for ever" into a track that ends, so it is the
    /// decoder's job rather than the chain's -- nothing downstream knows the
    /// difference between a track fading out and one that happens to get quiet.
    /// `volume` is applied too, which Cog does not do for USF; the tag is rare
    /// and the alternative is carrying a documented field that does nothing.
    void applyGain(std::int16_t* frames, std::size_t count) {
        const std::int64_t fadeLength = totalFrames_ - fadeStart_;
        const bool fading = fadeLength > 0 && framePos_ + static_cast<std::int64_t>(count) > fadeStart_;
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
                frames[i * kChannels + channel] = static_cast<std::int16_t>(
                    std::clamp(scaled, -32768.0, 32767.0));
            }
        }
    }

    const PluginRegistry* registry_ = nullptr;
    const Settings*       settings_ = nullptr;

    Url                       url_;
    std::unique_ptr<UsfState> state_;
    AudioFormat               format_{};
    int                       rate_        = kDefaultRate;
    std::int64_t              framePos_    = 0;
    std::int64_t              fadeStart_   = 0;
    std::int64_t              totalFrames_ = 0;
    double                    volume_      = 1.0;
    MetadataMap               tags_;

    std::vector<std::int16_t> scratch_;
    std::vector<std::int16_t> pending_;
    std::size_t               pendingPos_ = 0;
};

constexpr std::string_view kExtensions[] = {"usf", "miniusf"};

}  // namespace
}  // namespace xpcog

void xpcog_register_usf(xpcog::PluginRegistry& r) {
    // `usflib` is deliberately absent. A library is the game's program with no
    // track in it -- no length, no title, nothing to play -- and claiming it
    // would put one unplayable row in the playlist for every set scanned.
    r.addDecoder({
        .name       = "UsfDecoder",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kExtensions,
        .mimeTypes  = {},
        .create     = []() -> xpcog::DecoderPtr {
            return std::make_unique<xpcog::UsfDecoder>();
        },
        .available = nullptr,
    });
}
