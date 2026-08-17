#include "xpcog/core/AudioChunk.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace xpcog {

std::byte* AudioChunk::allocFrames(std::size_t frames) {
    const std::uint32_t stride = format_.bytesPerFrame();
    assert(stride > 0 && "AudioChunk::allocFrames needs a format first");
    data_.resize(frames * stride);
    return data_.data();
}

void AudioChunk::assign(const void* src, std::size_t frames) {
    std::byte* dst = allocFrames(frames);
    if (frames > 0 && src != nullptr) {
        std::memcpy(dst, src, frames * format_.bytesPerFrame());
    }
}

std::size_t AudioChunk::frameCount() const noexcept {
    const std::uint32_t stride = format_.bytesPerFrame();
    return (stride == 0) ? 0 : data_.size() / stride;
}

void AudioChunk::setFrameCount(std::size_t frames) {
    assert(frames <= frameCount() && "AudioChunk::setFrameCount only truncates");
    data_.resize(frames * format_.bytesPerFrame());
}

void AudioChunk::removeFrames(std::size_t frames, AudioChunk& out) {
    const std::size_t available = frameCount();
    const std::size_t taken     = std::min(frames, available);

    out.format_          = format_;
    out.lossless         = lossless;
    out.hdcd             = hdcd;
    out.dsdDoPReverseBits = dsdDoPReverseBits;
    out.resetForward     = resetForward;
    out.streamTimestamp  = streamTimestamp;
    out.streamTimeRatio  = streamTimeRatio;

    const std::uint32_t stride = format_.bytesPerFrame();
    const std::size_t   cut    = taken * stride;

    out.data_.assign(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(cut));
    data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(cut));

    // Whatever remains now starts later in the stream, and a reset only applies to
    // the leading edge -- so it travels with the piece that was removed.
    resetForward = false;
    if (stride > 0 && format_.sampleRate > 0.0) {
        streamTimestamp += static_cast<double>(taken) / format_.sampleRate;
    }
}

double AudioChunk::duration() const noexcept {
    if (format_.sampleRate <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(frameCount()) / format_.sampleRate;
}

double AudioChunk::durationRatioed() const noexcept {
    return duration() * streamTimeRatio;
}

AudioChunk AudioChunk::clone() const {
    AudioChunk copy;
    copy.format_           = format_;
    copy.data_             = data_;
    copy.lossless          = lossless;
    copy.hdcd              = hdcd;
    copy.dsdDoPReverseBits = dsdDoPReverseBits;
    copy.resetForward      = resetForward;
    copy.streamTimestamp   = streamTimestamp;
    copy.streamTimeRatio   = streamTimeRatio;
    return copy;
}

}  // namespace xpcog
