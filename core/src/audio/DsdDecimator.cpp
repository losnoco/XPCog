#include "xpcog/core/audio/DsdDecimator.hpp"

#include <dsd2pcm.h>

#include <cstdint>

namespace xpcog {

struct DsdDecimator::Filters {
    /// One per channel: the filter carries 64 taps of history, and a stereo
    /// stream's two channels are independent signals.
    std::vector<dsd2pcm_state*> channels;

    ~Filters() {
        for (dsd2pcm_state* filter : channels) {
            dsd2pcm_free(filter);
        }
    }
};

DsdDecimator::DsdDecimator() : filters_(std::make_unique<Filters>()) {}
DsdDecimator::~DsdDecimator() = default;

bool DsdDecimator::process(const AudioChunk& in, std::vector<float>& out) {
    const std::uint32_t channels = in.format().channels;
    if (in.format().format != SampleFormat::DSD || channels == 0) {
        return false;
    }
    while (filters_->channels.size() < channels) {
        dsd2pcm_state* filter = dsd2pcm_alloc();
        if (filter == nullptr) {
            return false;
        }
        filters_->channels.push_back(filter);
    }

    const std::size_t frames = in.frameCount();
    out.resize(frames * channels);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(in.bytes().data());
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        dsd2pcm_process(filters_->channels[channel], bytes, channel, channels, out.data(),
                        channel, channels, frames);
    }
    return true;
}

void DsdDecimator::reset() noexcept {
    for (dsd2pcm_state* filter : filters_->channels) {
        dsd2pcm_reset(filter);
    }
}

}  // namespace xpcog
