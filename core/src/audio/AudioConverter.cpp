#include "xpcog/core/audio/AudioConverter.hpp"

#include "xpcog/core/audio/Downmix.hpp"
#include "xpcog/core/audio/FreeSurround.hpp"

#include "xpcog/core/audio/SampleConvert.hpp"

#include <hdcd_decode2.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
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

struct AudioConverter::Hdcd {
    hdcd_state_stereo_t state{};
    bool                started = false;
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
    : soxr_(std::make_unique<Soxr>()), hdcd_(std::make_unique<Hdcd>()) {}
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

void AudioConverter::reset() {
    if (soxr_->handle != nullptr) {
        soxr_delete(soxr_->handle);
        soxr_->handle = nullptr;
    }
    inRate_     = 0.0;
    inChannels_ = 0;

    hdcd_->started = false;
    hdcdDetected_  = false;
    history_.clear();

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

    if (soxr_->handle != nullptr) {
        soxr_delete(soxr_->handle);
        soxr_->handle = nullptr;
    }

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
    return error == nullptr && soxr_->handle != nullptr;
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

    if (wantHdcd) {
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
    const float*      samples     = remapped_.data();
    std::size_t       frameCount  = frames;

    if (soxr_->handle != nullptr) {
        // Ratio plus a margin: soxr can emit slightly more than the nominal
        // count on any given call.
        const double      ratio    = outRate_ / inFormat.sampleRate;
        const std::size_t capacity = static_cast<std::size_t>(
                                         std::ceil(static_cast<double>(frames) * ratio)) +
                                     64;
        resampled_.resize(capacity * chainChannels());

        std::size_t consumed = 0;
        std::size_t produced = 0;
        const soxr_error_t error =
            soxr_process(soxr_->handle, remapped_.data(), frames, &consumed,
                         resampled_.data(), capacity, &produced);
        if (error != nullptr) {
            return false;
        }
        samples    = resampled_.data();
        frameCount = produced;
    }

    // 4. gain, then out -- through the upmixer if one is running.
    //
    // Gain first, which is Cog's order: ReplayGain is applied in ConverterNode
    // and the surround decoder is a DSP node downstream of it. It matters, because
    // the decoder's steering is not scale-invariant -- the epsilon test that
    // decides whether a bin has a decodable position at all compares against an
    // absolute amplitude.
    if (fsurround_ == nullptr) {
        appendWithGain(samples, frameCount, out);
        return true;
    }

    fsGained_.resize(frameCount * 2);
    if (gain_ == 1.0F) {
        std::copy_n(samples, fsGained_.size(), fsGained_.begin());
    } else {
        for (std::size_t i = 0; i < fsGained_.size(); ++i) {
            fsGained_[i] = samples[i] * gain_;
        }
    }
    pushFreeSurround(fsGained_.data(), frameCount, out);
    return true;
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
        // Flushing means feeding null until soxr stops producing; otherwise the
        // tail held in its filter delay line is simply lost.
        for (;;) {
            constexpr std::size_t kChunk = 4096;
            resampled_.resize(kChunk * chainChannels());

            std::size_t        produced = 0;
            const soxr_error_t error =
                soxr_process(soxr_->handle, nullptr, 0, nullptr, resampled_.data(),
                             kChunk, &produced);
            if (error != nullptr || produced == 0) {
                break;
            }

            if (fsurround_ == nullptr) {
                appendWithGain(resampled_.data(), produced, out);
                continue;
            }

            fsGained_.resize(produced * 2);
            if (gain_ == 1.0F) {
                std::copy_n(resampled_.data(), fsGained_.size(), fsGained_.begin());
            } else {
                for (std::size_t i = 0; i < fsGained_.size(); ++i) {
                    fsGained_[i] = resampled_[i] * gain_;
                }
            }
            pushFreeSurround(fsGained_.data(), produced, out);
        }
    }

    // After the resampler, and unconditionally: the upmixer holds half a block
    // whether or not anything was being resampled, and the early return this
    // function used to open with would have dropped it on every same-rate track.
    if (fsurround_ != nullptr) {
        flushFreeSurround(out);
    }
}

}  // namespace xpcog
