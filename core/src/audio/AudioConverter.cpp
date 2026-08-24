#include "xpcog/core/audio/AudioConverter.hpp"

#include "xpcog/core/audio/Downmix.hpp"
#include "xpcog/core/audio/FreeSurround.hpp"

#include "xpcog/core/audio/SampleConvert.hpp"

#include <dsd2pcm.h>
#include <hdcd_decode2.h>
#include <lpc.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace xpcog {
namespace {

[[nodiscard]] soxr_quality_spec_t qualityFor(std::string_view name) {
    unsigned long recipe = SOXR_HQ;
    if (name == "quick") {
        recipe = SOXR_QQ;
    } else if (name == "low") {
        recipe = SOXR_LQ;
    } else if (name == "medium") {
        recipe = SOXR_MQ;
    } else if (name == "best") {
        recipe = SOXR_VHQ;
    }
    return soxr_quality_spec(recipe, 0);
}

/// Fits `inChannels` into `outChannels`.
///
/// Reducing to stereo or mono goes through the real matrix (see Downmix.hpp),
/// which is where M4 put what an earlier comment here expected to become a
/// DSPDownmixNode -- downmix changes the frame size, and a DSPNode transforms in
/// place at a fixed format.
///
/// The growing direction routes by channel flag (see upmix in Downmix.hpp), so a
/// quad file on a 5.1 device keeps its back pair at the back instead of leaking
/// into the centre and LFE slots, which is what positional copying did.
///
/// The one remaining case -- reducing to a multichannel layout, say 7.1 into
/// quad -- keeps the positional copy below. Cog reduces through its stereo
/// matrix regardless of the target; doing that here would discard a real quad
/// device's back speakers, so the plain copy stays until that reduction has a
/// matrix of its own.
void fitChannels(const float* in, std::size_t frames, std::uint32_t inChannels,
                 std::uint32_t inConfig, std::uint32_t outChannels,
                 std::vector<float>& out) {
    out.resize(frames * outChannels);

    if (inChannels == outChannels) {
        std::copy_n(in, frames * outChannels, out.begin());
        return;
    }

    if (inChannels > outChannels && outChannels == 2) {
        downmixToStereo(in, inChannels, inConfig, out.data(), frames);
        return;
    }
    if (inChannels > outChannels && outChannels == 1) {
        downmixToMono(in, inChannels, inConfig, out.data(), frames);
        return;
    }

    if (inChannels < outChannels) {
        upmix(in, inChannels, inConfig, out.data(), outChannels,
              guessChannelConfig(outChannels), frames);
        return;
    }

    const std::uint32_t common = std::min(inChannels, outChannels);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < common; ++c) {
            out[f * outChannels + c] = in[f * inChannels + c];
        }
        for (std::uint32_t c = common; c < outChannels; ++c) {
            out[f * outChannels + c] = 0.0F;
        }
    }
}

/// Euclid, on the sample rates.
[[nodiscard]] unsigned greatestCommonDivisor(unsigned a, unsigned b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    unsigned c = a % b;
    while (c != 0) {
        a = b;
        b = c;
        c = a % b;
    }
    return b;
}

/// How many input frames to predict at each edge of a resampler run, and how
/// many output frames that comes back as. Cog's samples_len (lvqcl), which is
/// the whole reason the trim can be exact.
///
/// Taking the rates as a fraction and reducing it gives the smallest pair of
/// whole numbers in the same proportion -- 44100:48000 is 147:160, a 300th of a
/// second. Any whole multiple of that pair is still exact, so the multiple is
/// chosen for duration: about a twentieth of a second, which is what the
/// prediction is worth acoustically, then capped so neither side exceeds 8192
/// frames. The point of all of it is that `out` is precisely what `in` becomes.
/// Round the two independently instead and the error is a fraction of a frame
/// per track, which is exactly the kind of thing that stops a seam being
/// sample-accurate.
struct Padding {
    std::size_t in  = 0;
    std::size_t out = 0;
};

[[nodiscard]] Padding paddingFor(double inRate, double outRate) {
    if (inRate <= 0.0 || outRate <= 0.0) {
        return {};
    }

    constexpr unsigned kPerSecond = 20;    // a twentieth of a second
    constexpr unsigned kMaxFrames = 8192;  // and no more than this either side

    auto r1 = static_cast<unsigned>(inRate);
    auto r2 = static_cast<unsigned>(outRate);

    const unsigned divisor = greatestCommonDivisor(r1, r2);
    if (divisor == 0) {
        return {};
    }
    r1 /= divisor;
    r2 /= divisor;

    unsigned       multiple = (divisor + kPerSecond - 1) / kPerSecond;
    const unsigned longer   = std::max(r1, r2);
    if (longer * multiple > kMaxFrames) {
        multiple = kMaxFrames / longer;
    }
    if (multiple < 1) {
        multiple = 1;
    }

    return {static_cast<std::size_t>(r1) * multiple,
            static_cast<std::size_t>(r2) * multiple};
}

/// How much real signal the prediction is fitted to: a twentieth of a second,
/// floored at 1024 frames and capped at 16384, and never shorter than the
/// filter it has to estimate. Cog's PRIME_LEN_.
[[nodiscard]] std::size_t primeLengthFor(double inRate) {
    auto prime = static_cast<std::size_t>(inRate / 20.0);
    prime      = std::max<std::size_t>(prime, 1024);
    prime      = std::min<std::size_t>(prime, 16384);
    return std::max<std::size_t>(prime, (2 * LPC_ORDER) + 1);
}

}  // namespace

/// Cog's block size, and not a tuning knob: FreeSurround decodes positions at a
/// granularity that follows from it, and upstream is explicit that anything
/// outside 5 to 20 ms of single-channel samples changes the result rather than
/// just the cost.
constexpr unsigned kFreeSurroundBlock = 4096;

/// FreeSurround emits FL, FC, FR, BL, BR, LFE; XPCog interleaves by channel-flag
/// order, which for 5.1 is FL, FR, FC, LFE, BL, BR. Indexed by the decoder's
/// channel, giving ours.
constexpr unsigned kFreeSurroundToLayout[] = {0, 2, 1, 4, 5, 3};

class AudioConverter::FreeSurroundStage {
public:
    explicit FreeSurroundStage(double sampleRate)
        : decoder(cs_5point1, kFreeSurroundBlock) {
        // Cog's settings, from freesurround_params in FSurroundFilter.mm, not
        // the decoder's own defaults -- the two differ, and this is the
        // configuration the golden capture was taken at.
        decoder.setCircularWrap(90.0F);
        decoder.setShift(0.0F);
        decoder.setDepth(1.0F);
        decoder.setFocus(0.0F);
        decoder.setCenterImage(0.7F);
        decoder.setFrontSeparation(1.0F);
        decoder.setRearSeparation(1.0F);
        decoder.setBassRedirection(false);
        decoder.setLowCutoff(static_cast<float>(40.0 / (sampleRate / 2.0)));
        decoder.setHighCutoff(static_cast<float>(90.0 / (sampleRate / 2.0)));
    }

    FreeSurround decoder;
};

struct AudioConverter::DsdFilters {
    /// One per channel: the filter carries 64 taps of history, and a stereo
    /// stream's two channels are independent signals.
    std::vector<dsd2pcm_state*> channels;

    ~DsdFilters() {
        for (dsd2pcm_state* filter : channels) {
            dsd2pcm_free(filter);
        }
    }
};

struct AudioConverter::Hdcd {
    hdcd_state_stereo_t state{};
    bool                started = false;
};

struct AudioConverter::LpcScratch {
    /// Grown in place by lpc_extrapolate2 through realloc, so it is malloc'd
    /// memory and not a vector.
    void*       buffer = nullptr;
    std::size_t size   = 0;

    ~LpcScratch() { std::free(buffer); }
};

struct AudioConverter::Soxr {
    soxr_t handle = nullptr;
    ~Soxr() {
        if (handle != nullptr) {
            soxr_delete(handle);
        }
    }
};

AudioConverter::AudioConverter()
    : soxr_(std::make_unique<Soxr>()), hdcd_(std::make_unique<Hdcd>()),
      lpc_(std::make_unique<LpcScratch>()) {}
AudioConverter::~AudioConverter() = default;

bool AudioConverter::setOutputFormat(double sampleRate, std::uint32_t channels,
                                     std::string_view quality) {
    if (sampleRate <= 0.0 || channels == 0) {
        return false;
    }
    outRate_     = sampleRate;
    outChannels_ = channels;
    quality_     = std::string{quality};
    // After the rate is known, because the crossover frequencies are given to
    // the decoder normalised to Nyquist.
    configureFreeSurround();
    reset();
    return true;
}

void AudioConverter::setFreeSurround(bool enabled) {
    freeSurroundWanted_ = enabled;
    configureFreeSurround();
    reset();
}

bool AudioConverter::freeSurroundEnabled() const noexcept {
    return fsurround_ != nullptr;
}

void AudioConverter::configureFreeSurround() {
    // Six channels or nothing. The upmixer produces one layout, and asking for
    // it while the device is running some other width would silently produce
    // interleaving nobody wants -- better to leave it off and be a plain
    // converter than to half-apply it.
    const bool usable = freeSurroundWanted_ && outRate_ > 0.0 &&
                        outChannels_ == FreeSurround::channelCount(cs_5point1);
    if (!usable) {
        fsurround_.reset();
        return;
    }
    fsurround_ = std::make_unique<FreeSurroundStage>(outRate_);
}

std::uint32_t AudioConverter::chainChannels() const noexcept {
    return fsurround_ != nullptr ? 2U : outChannels_;
}

void AudioConverter::closeResampler() noexcept {
    if (soxr_->handle != nullptr) {
        soxr_delete(soxr_->handle);
        soxr_->handle = nullptr;
    }
    // Forgotten, not merely deleted: configureFor() decides whether it can keep
    // the existing instance by comparing against these, so leaving them set
    // would have it keep an instance that is no longer there.
    inRate_     = 0.0;
    inChannels_ = 0;

    // The edges belong to the instance. A fresh resampler starts with no
    // history again, so it needs its run-up predicted again, and the trim that
    // takes the run-up back off has to be re-armed with it.
    padIn_        = 0;
    padOut_       = 0;
    primeLen_     = 0;
    leadInDone_   = false;
    latencyEaten_ = 0;
    history_.clear();
}

void AudioConverter::reset() {
    closeResampler();

    hdcd_->started = false;
    hdcdDetected_  = false;

    // The filters keep 64 taps of the old position, and a seek makes those the
    // wrong 64 taps. Reset rather than freed: rebuilding means recomputing the
    // lookup tables, and the far side of a seek is the same DSD stream.
    if (dsd_ != nullptr) {
        for (dsd2pcm_state* filter : dsd_->channels) {
            dsd2pcm_reset(filter);
        }
    }

    if (fsurround_ != nullptr) {
        fsurround_->decoder.flush();
        // Re-armed, not merely cleared. After a seek the decoder is primed from
        // scratch again, so the half block of silence it produces first has to
        // be dropped again -- otherwise every seek would insert 46 ms of it.
        fsSkip_ = fsurround_->decoder.blockSize() / 2;
    }
    fsPending_.clear();
    fsOwed_ = 0;
}

bool AudioConverter::configureFor(const AudioFormat& input) {
    if (soxr_->handle != nullptr && input.sampleRate == inRate_ &&
        input.channels == inChannels_) {
        return true;
    }

    closeResampler();

    inRate_     = input.sampleRate;
    inChannels_ = input.channels;

    // Matching rates need no resampler at all -- and skipping it keeps the path
    // bit-exact, which the resampler null test relies on.
    if (input.sampleRate == outRate_) {
        return true;
    }

    soxr_io_spec_t      io      = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t quality = qualityFor(quality_);

    soxr_error_t error = nullptr;
    // The chain's width, not the device's: with the upmixer on there are two
    // channels to resample here rather than six.
    soxr_->handle = soxr_create(input.sampleRate, outRate_, chainChannels(), &error,
                                &io, &quality, nullptr);
    if (error != nullptr || soxr_->handle == nullptr) {
        return false;
    }

    const Padding padding = paddingFor(input.sampleRate, outRate_);
    padIn_                = padding.in;
    padOut_               = padding.out;
    primeLen_             = primeLengthFor(input.sampleRate);
    return true;
}

bool AudioConverter::decimateDsd(const AudioChunk& in, std::size_t frames) {
    const std::uint32_t channels = in.format().channels;
    if (channels == 0) {
        return false;
    }

    if (dsd_ == nullptr) {
        dsd_ = std::make_unique<DsdFilters>();
    }
    while (dsd_->channels.size() < channels) {
        dsd2pcm_state* filter = dsd2pcm_alloc();
        if (filter == nullptr) {
            return false;
        }
        dsd_->channels.push_back(filter);
    }

    // One byte of DSD is eight one-bit samples and becomes one float, which is
    // where the eight-to-one decimation happens and why the rate does not
    // change here: a chunk that arrives at 705,600 Hz leaves at 705,600 Hz,
    // and the resampler downstream takes it to the device's rate.
    decoded_.resize(frames * channels);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(in.bytes().data());
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        dsd2pcm_process(dsd_->channels[channel], bytes, channel, channels,
                        decoded_.data(), channel, channels, frames);
    }

    if (halveDsd_) {
        for (float& sample : decoded_) {
            sample *= 0.5F;
        }
    }
    return true;
}

bool AudioConverter::process(const AudioChunk& in, std::vector<float>& out) {
    if (outChannels_ == 0 || outRate_ <= 0.0) {
        return false;
    }

    const std::size_t frames = in.frameCount();
    if (frames == 0) {
        return true;
    }

    // 1. HDCD, before anything else -- it operates on the 16-bit integers the
    //    codes are carried in, and it is stateful across chunks, which is why it
    //    lives here rather than in the stateless sample conversion.
    const AudioFormat& inFormat = in.format();
    // HDCD is a Red Book CD format: 16-bit, 44.1 kHz, stereo, and only meaningful
    // in lossless material. Gating on all four avoids chasing false positives
    // through content that cannot carry the codes.
    const bool wantHdcd = hdcdEnabled_ && inFormat.format == SampleFormat::S16 &&
                          inFormat.channels == 2 && in.lossless &&
                          inFormat.sampleRate == 44100.0;

    if (inFormat.format == SampleFormat::DSD) {
        // Before everything, like HDCD below and for the same reason: it is
        // stateful across chunks, so it cannot live in the stateless sample
        // conversion. What it produces is ordinary float and the rest of this
        // function does not know the difference.
        if (!decimateDsd(in, frames)) {
            return false;
        }
    } else if (wantHdcd) {
        if (!hdcd_->started) {
            hdcd_reset_stereo(&hdcd_->state,
                              static_cast<unsigned>(inFormat.sampleRate));
            hdcd_->started = true;
        }

        const std::size_t samples = frames * 2;
        hdcdSamples_.resize(samples);

        const auto* source = reinterpret_cast<const std::int16_t*>(in.bytes().data());
        for (std::size_t i = 0; i < samples; ++i) {
            hdcdSamples_[i] = source[i];
        }

        hdcd_process_stereo(&hdcd_->state, hdcdSamples_.data(),
                            static_cast<int>(frames));

        if (!hdcdDetected_) {
            // Only worth asking until the answer is yes; it never reverts.
            hdcd_detection_data_t detect{};
            hdcd_detect_reset(&detect);
            hdcd_detect_recalc_stereo(&hdcd_->state, &detect);
            hdcdDetected_ = detect.hdcd_detected != 0;
        }

        // hdcd_process_stereo scales its 16-bit input by 2^15, so full scale
        // lands at 2^30 rather than 2^15. Measured, not assumed: -32768 comes
        // back as exactly -2^30, which makes this dividing step bit-transparent
        // for material carrying no HDCD codes at all. Decoded HDCD peak
        // extension deliberately exceeds 1.0 here and is brought back by the
        // gain adjustment the decoder already applied.
        decoded_.resize(samples);
        constexpr float kHdcdScale = 1.0F / 1073741824.0F;  // 2^30
        for (std::size_t i = 0; i < samples; ++i) {
            decoded_[i] = static_cast<float>(hdcdSamples_[i]) * kHdcdScale;
        }
    } else {
        // 1b. decoder output -> float32
        decoded_.resize(float32SampleCount(in));
        if (convertToFloat32(in, decoded_) != decoded_.size()) {
            return false;  // a layout with no float conversion (raw DSD)
        }
    }

    // 2. fit channels -- to the chain's width, which is stereo when the upmixer
    //    is going to widen it again at the end.
    fitChannels(decoded_.data(), frames, inFormat.channels, inFormat.channelConfig,
                chainChannels(), remapped_);

    if (!configureFor(inFormat)) {
        return false;
    }

    // 3. resample, or pass straight through when the rates already agree
    const float* samples    = remapped_.data();
    std::size_t  frameCount = frames;

    if (soxr_->handle != nullptr) {
        const float* feed       = remapped_.data();
        std::size_t  feedFrames = frames;

        // The first block of the run reaches the filter with nothing in front
        // of it. Give it something.
        if (!leadInDone_ && padIn_ > 0) {
            extrapolateLeadIn(frames);
            feed       = padded_.data();
            feedFrames = padIn_ + frames;
        }

        resampled_.clear();
        if (!resampleInto(feed, feedFrames, resampled_)) {
            return false;
        }

        // The real input only, not the prediction: what the far edge gets
        // extrapolated from has to be signal that was actually in the file.
        rememberTail(remapped_.data(), frames);

        // And take the run-up back off, now that it has done its work in the
        // filter. What comes out is aligned with what went in.
        eatLeadIn(resampled_);

        samples    = resampled_.data();
        frameCount = resampled_.size() / chainChannels();
    }

    emitFrames(samples, frameCount, out);
    return true;
}

void AudioConverter::emitFrames(const float* samples, std::size_t frames,
                                std::vector<float>& out) {
    // Gain first, which is Cog's order: ReplayGain is applied in ConverterNode
    // and the surround decoder is a DSP node downstream of it. It matters, because
    // the decoder's steering is not scale-invariant -- the epsilon test that
    // decides whether a bin has a decodable position at all compares against an
    // absolute amplitude.
    if (fsurround_ == nullptr) {
        appendWithGain(samples, frames, out);
        return;
    }

    fsGained_.resize(frames * 2);
    if (gain_ == 1.0F) {
        std::copy_n(samples, fsGained_.size(), fsGained_.begin());
    } else {
        for (std::size_t i = 0; i < fsGained_.size(); ++i) {
            fsGained_[i] = samples[i] * gain_;
        }
    }
    pushFreeSurround(fsGained_.data(), frames, out);
}

bool AudioConverter::resampleInto(const float* input, std::size_t frames,
                                  std::vector<float>& out) {
    const std::size_t channels = chainChannels();
    const double      ratio    = outRate_ / inRate_;

    std::size_t offset = 0;
    while (offset < frames) {
        const std::size_t remaining = frames - offset;
        // The nominal count, plus what is sitting in the delay line, plus a
        // margin: soxr can emit slightly more than the ratio suggests on any
        // given call, and a short buffer would leave input unconsumed.
        const std::size_t capacity =
            static_cast<std::size_t>(std::ceil(static_cast<double>(remaining) * ratio)) +
            static_cast<std::size_t>(std::ceil(soxr_delay(soxr_->handle))) + 64;

        const std::size_t base = out.size();
        out.resize(base + (capacity * channels));

        std::size_t        consumed = 0;
        std::size_t        produced = 0;
        const soxr_error_t error =
            soxr_process(soxr_->handle, input + (offset * channels), remaining,
                         &consumed, out.data() + base, capacity, &produced);
        out.resize(base + (produced * channels));
        if (error != nullptr) {
            return false;
        }
        // No progress and no error means nothing more is coming; looping again
        // would spin rather than finish.
        if (consumed == 0 && produced == 0) {
            break;
        }
        offset += consumed;
    }
    return true;
}

void AudioConverter::extrapolateLeadIn(std::size_t frames) {
    const std::size_t channels = chainChannels();

    padded_.assign((padIn_ + frames) * channels, 0.0F);
    std::copy_n(remapped_.data(), frames * channels,
                padded_.begin() + static_cast<std::ptrdiff_t>(padIn_ * channels));

    // Writes backwards into the headroom left in front of it, which is why the
    // pointer handed over is the start of the real data rather than the buffer.
    lpc_extrapolate_bkwd(padded_.data() + (padIn_ * channels), frames,
                         std::min(frames, primeLen_), static_cast<int>(channels),
                         static_cast<int>(LPC_ORDER), padIn_, &lpc_->buffer,
                         &lpc_->size);

    latencyEaten_ = padOut_;
    leadInDone_   = true;
}

std::size_t AudioConverter::pushLeadOut(std::vector<float>& out) {
    const std::size_t channels = chainChannels();
    if (padIn_ == 0 || history_.empty()) {
        return 0;
    }

    const std::size_t primed = history_.size() / channels;
    padded_.assign((primed + padIn_) * channels, 0.0F);
    std::copy_n(history_.data(), primed * channels, padded_.begin());

    lpc_extrapolate_fwd(padded_.data(), primed, std::min(primed, primeLen_),
                        static_cast<int>(channels), static_cast<int>(LPC_ORDER),
                        padIn_, &lpc_->buffer, &lpc_->size);

    // Only the predicted part goes in -- the history it was fitted to is
    // already through the resampler and would be heard twice.
    if (!resampleInto(padded_.data() + (primed * channels), padIn_, out)) {
        return 0;
    }
    return padOut_;
}

void AudioConverter::rememberTail(const float* input, std::size_t frames) {
    if (primeLen_ == 0) {
        return;
    }
    const std::size_t channels = chainChannels();
    history_.insert(history_.end(), input, input + (frames * channels));

    const std::size_t held = history_.size() / channels;
    if (held > primeLen_) {
        history_.erase(history_.begin(),
                       history_.begin() +
                           static_cast<std::ptrdiff_t>((held - primeLen_) * channels));
    }
}

void AudioConverter::eatLeadIn(std::vector<float>& buffer) {
    if (latencyEaten_ == 0) {
        return;
    }
    const std::size_t channels = chainChannels();
    // A block shorter than the trim leaves the rest owed. Rare -- the trim is a
    // twentieth of a second -- but a very short track is exactly the case that
    // would otherwise emit the prediction as audio.
    const std::size_t drop = std::min(latencyEaten_, buffer.size() / channels);
    buffer.erase(buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(drop * channels));
    latencyEaten_ -= drop;
}

void AudioConverter::appendWithGain(const float* samples, std::size_t frames,
                                    std::vector<float>& out) {
    const std::size_t offset = out.size();
    const std::size_t count  = frames * chainChannels();
    out.resize(offset + count);

    if (gain_ == 1.0F) {
        std::copy_n(samples, count, out.begin() + static_cast<std::ptrdiff_t>(offset));
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            out[offset + i] = samples[i] * gain_;
        }
    }
}

void AudioConverter::pushFreeSurround(const float* stereo, std::size_t frames,
                                      std::vector<float>& out) {
    const std::size_t block = fsurround_->decoder.blockSize();

    fsOwed_ += frames;
    fsPending_.insert(fsPending_.end(), stereo, stereo + (frames * 2));

    // Whole blocks only. The decoder takes exactly one or nothing, and feeding
    // it a zero-padded short block mid-stream would put a gap in the overlap-add
    // rather than a gap in the sound -- which is worse, because it would be
    // audible everywhere except where it was introduced.
    while (fsPending_.size() >= block * 2) {
        emitFreeSurroundBlock(fsPending_.data(), out);
        fsPending_.erase(fsPending_.begin(),
                         fsPending_.begin() + static_cast<std::ptrdiff_t>(block * 2));
    }
}

void AudioConverter::emitFreeSurroundBlock(const float* stereoBlock,
                                           std::vector<float>& out) {
    const std::size_t block   = fsurround_->decoder.blockSize();
    const float*      decoded = fsurround_->decoder.decode(stereoBlock);

    std::size_t start = 0;
    if (fsSkip_ > 0) {
        start = std::min(fsSkip_, block);
        fsSkip_ -= start;
    }

    const std::size_t take = std::min(block - start, fsOwed_);
    if (take == 0) {
        return;
    }

    const std::size_t offset = out.size();
    out.resize(offset + (take * outChannels_));
    for (std::size_t f = 0; f < take; ++f) {
        const float* source = decoded + ((start + f) * outChannels_);
        float*       target = out.data() + offset + (f * outChannels_);
        for (std::uint32_t c = 0; c < outChannels_; ++c) {
            target[kFreeSurroundToLayout[c]] = source[c];
        }
    }
    fsOwed_ -= take;
}

void AudioConverter::flushFreeSurround(std::vector<float>& out) {
    const std::size_t block = fsurround_->decoder.blockSize();

    // What is owed at end of stream is the partial block plus the half block the
    // decoder is holding back, so at most two more blocks come out. Zeros are
    // the right padding here and only here: there is no audio after this, so the
    // overlap-add has nothing left to be discontinuous with.
    for (int guard = 0; fsOwed_ > 0 && guard < 4; ++guard) {
        fsPending_.resize(block * 2, 0.0F);
        emitFreeSurroundBlock(fsPending_.data(), out);
        fsPending_.clear();
    }
}

void AudioConverter::drain(std::vector<float>& out) {
    if (soxr_->handle != nullptr) {
        const std::size_t channels = chainChannels();
        resampled_.clear();

        // Before the flush, not after: the resampler is still open, and what it
        // wants at the closing edge is the same thing it wanted at the opening
        // one -- signal beyond the boundary to convolve against, rather than the
        // drop to silence that is all the end of a file otherwise offers.
        const std::size_t eatPost = pushLeadOut(resampled_);

        // Flushing means feeding null until soxr stops producing; otherwise the
        // tail held in its filter delay line is simply lost.
        for (;;) {
            constexpr std::size_t kChunk = 4096;

            const std::size_t base = resampled_.size();
            resampled_.resize(base + (kChunk * channels));

            std::size_t        produced = 0;
            const soxr_error_t error =
                soxr_process(soxr_->handle, nullptr, 0, nullptr,
                             resampled_.data() + base, kChunk, &produced);
            resampled_.resize(base + (produced * channels));
            if (error != nullptr || produced == 0) {
                break;
            }
        }

        // A track shorter than the front trim never finished paying it, and the
        // flush is the last chance to.
        eatLeadIn(resampled_);

        // Then the far edge. eatPost is what the prediction became on the way
        // through -- exactly, because the pair it was sized from is exact -- so
        // taking that many frames off the end leaves the last real sample last.
        const std::size_t drained = resampled_.size() / channels;
        resampled_.resize((drained - std::min(eatPost, drained)) * channels);

        emitFrames(resampled_.data(), resampled_.size() / channels, out);
    }

    // Flushing is terminal for a soxr instance: once it has been fed the null
    // input that signals end of stream, handing it more audio is undefined, and
    // in practice it crashes. That is the whole reason this is here rather than
    // left to configureFor() -- a track seam drains, and if the incoming track
    // happens to share the outgoing one's rate and channel count then
    // configureFor() sees nothing to rebuild and reuses the drained instance.
    // Same rate across a seam is the common case, not the exotic one.
    closeResampler();

    // After the resampler, and unconditionally: the upmixer holds half a block
    // whether or not anything was being resampled, and the early return this
    // function used to open with would have dropped it on every same-rate track.
    if (fsurround_ != nullptr) {
        flushFreeSurround(out);
    }
}

}  // namespace xpcog
