#include "xpcog/core/audio/AudioTap.hpp"

#include <algorithm>

namespace xpcog {
namespace {

std::size_t roundUpToPowerOfTwo(std::size_t value) {
    std::size_t rounded = 1;
    while (rounded < value) {
        rounded <<= 1U;
    }
    return rounded;
}

}  // namespace

AudioTap::AudioTap(std::size_t capacityFrames)
    : capacity_(roundUpToPowerOfTwo(std::max<std::size_t>(capacityFrames, 2))),
      mask_(capacity_ - 1),
      samples_(capacity_) {
    for (std::atomic<float>& sample : samples_) {
        sample.store(0.0F, std::memory_order_relaxed);
    }
}

void AudioTap::write(const float* samples, std::size_t count,
                     std::size_t channels) noexcept {
    if (samples == nullptr || count == 0 || channels == 0) {
        return;
    }

    const std::size_t frames = count / channels;
    std::uint64_t     cursor = written_.load(std::memory_order_relaxed);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        // The average, not the first channel and not the max. A visualiser showing
        // only the left channel is wrong in an obvious way; showing the max
        // exaggerates anything hard-panned, which is most of a 1970s stereo mix.
        float sum = 0.0F;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sum += samples[(frame * channels) + channel];
        }

        samples_[static_cast<std::size_t>(cursor) & mask_].store(
            sum / static_cast<float>(channels), std::memory_order_relaxed);
        ++cursor;
    }

    // Published last, with release ordering, so a reader that sees this count also
    // sees the samples it counts. Without that the newest frames of every window
    // would be whatever the slots held previously -- which is the *oldest* audio in
    // the buffer, a second ago, and would read as a spectrum that stutters.
    written_.store(cursor, std::memory_order_release);
}

bool AudioTap::readLatest(float* out, std::size_t count) const noexcept {
    if (out == nullptr || count == 0) {
        return false;
    }

    const std::uint64_t cursor = written_.load(std::memory_order_acquire);
    if (cursor == 0) {
        return false;
    }

    // Never reach further back than the buffer holds: past that the slots have been
    // overwritten with *newer* audio, so reading them would splice the future into
    // the start of the window.
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(cursor, capacity_));
    const std::size_t wanted = std::min(count, available);
    const std::size_t pad    = count - wanted;

    // What is missing goes at the front, so the newest sample stays at the end where
    // the caller expects it.
    std::fill_n(out, pad, 0.0F);

    const std::uint64_t begin = cursor - wanted;
    for (std::size_t index = 0; index < wanted; ++index) {
        out[pad + index] =
            samples_[static_cast<std::size_t>(begin + index) & mask_].load(
                std::memory_order_relaxed);
    }
    return true;
}

void AudioTap::clear() noexcept {
    written_.store(0, std::memory_order_release);
    for (std::atomic<float>& sample : samples_) {
        sample.store(0.0F, std::memory_order_relaxed);
    }
}

}  // namespace xpcog
