#include "xpcog/core/audio/AudioConverter.hpp"

#include "xpcog/core/audio/Downmix.hpp"

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
    reset();
    return true;
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
    soxr_->handle = soxr_create(input.sampleRate, outRate_, outChannels_, &error, &io,
                                &quality, nullptr);
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

    // 2. fit channels
    fitChannels(decoded_.data(), frames, inFormat.channels, inFormat.channelConfig,
                outChannels_, remapped_);

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
        resampled_.resize(capacity * outChannels_);

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

    // 4. gain, appended to the caller's buffer
    const std::size_t offset  = out.size();
    const std::size_t samples_ = frameCount * outChannels_;
    out.resize(offset + samples_);

    if (gain_ == 1.0F) {
        std::copy_n(samples, samples_, out.begin() + static_cast<std::ptrdiff_t>(offset));
    } else {
        for (std::size_t i = 0; i < samples_; ++i) {
            out[offset + i] = samples[i] * gain_;
        }
    }
    return true;
}

void AudioConverter::drain(std::vector<float>& out) {
    if (soxr_->handle == nullptr) {
        return;
    }

    // Flushing means feeding null until soxr stops producing; otherwise the tail
    // held in its filter delay line is simply lost.
    for (;;) {
        constexpr std::size_t kChunk = 4096;
        resampled_.resize(kChunk * outChannels_);

        std::size_t        produced = 0;
        const soxr_error_t error =
            soxr_process(soxr_->handle, nullptr, 0, nullptr, resampled_.data(), kChunk,
                         &produced);
        if (error != nullptr || produced == 0) {
            return;
        }

        const std::size_t offset  = out.size();
        const std::size_t samples = produced * outChannels_;
        out.resize(offset + samples);

        if (gain_ == 1.0F) {
            std::copy_n(resampled_.data(), samples,
                        out.begin() + static_cast<std::ptrdiff_t>(offset));
        } else {
            for (std::size_t i = 0; i < samples; ++i) {
                out[offset + i] = resampled_[i] * gain_;
            }
        }
    }
}

}  // namespace xpcog
