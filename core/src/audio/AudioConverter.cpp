#include "xpcog/core/audio/AudioConverter.hpp"

#include "xpcog/core/audio/SampleConvert.hpp"

#include <soxr.h>

#include <algorithm>
#include <cmath>

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
/// Deliberately simple: copy matching channels, duplicate mono into every output,
/// and drop or zero the rest. A proper matrix downmix is DSPDownmixNode's job in
/// M4 -- doing it badly here would be worse than doing it plainly.
void fitChannels(const float* in, std::size_t frames, std::uint32_t inChannels,
                 std::uint32_t outChannels, std::vector<float>& out) {
    out.resize(frames * outChannels);

    if (inChannels == outChannels) {
        std::copy_n(in, frames * outChannels, out.begin());
        return;
    }

    if (inChannels == 1) {
        for (std::size_t f = 0; f < frames; ++f) {
            const float sample = in[f];
            for (std::uint32_t c = 0; c < outChannels; ++c) {
                out[f * outChannels + c] = sample;
            }
        }
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

struct AudioConverter::Soxr {
    soxr_t handle = nullptr;
    ~Soxr() {
        if (handle != nullptr) {
            soxr_delete(handle);
        }
    }
};

AudioConverter::AudioConverter() : soxr_(std::make_unique<Soxr>()) {}
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

    // 1. decoder output -> float32
    decoded_.resize(float32SampleCount(in));
    if (convertToFloat32(in, decoded_) != decoded_.size()) {
        return false;  // a layout with no float conversion (raw DSD)
    }

    // 2. fit channels
    fitChannels(decoded_.data(), frames, in.format().channels, outChannels_, remapped_);

    if (!configureFor(in.format())) {
        return false;
    }

    // 3. resample, or pass straight through when the rates already agree
    const float*      samples     = remapped_.data();
    std::size_t       frameCount  = frames;

    if (soxr_->handle != nullptr) {
        // Ratio plus a margin: soxr can emit slightly more than the nominal
        // count on any given call.
        const double      ratio    = outRate_ / in.format().sampleRate;
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
