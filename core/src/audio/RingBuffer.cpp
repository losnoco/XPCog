#include "xpcog/core/audio/RingBuffer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace xpcog {
namespace {

[[nodiscard]] std::size_t roundUpPow2(std::size_t n) noexcept {
    if (n < 2) {
        return 2;
    }
    return std::bit_ceil(n);
}

/// Rounds a short take down to whole frames. Only a *short* one: a take that
/// satisfies the request in full is already frame-aligned if the caller's own
/// count was, and rounding it would refuse a caller that legitimately deals in
/// something other than frames.
[[nodiscard]] std::size_t wholeFrames(std::size_t take, std::size_t wanted,
                                      std::size_t frame) noexcept {
    if (take >= wanted || frame < 2) {
        return take;
    }
    return take - (take % frame);
}

}  // namespace

RingBuffer::RingBuffer(std::size_t capacity)
    : capacity_(roundUpPow2(capacity + 1)),
      mask_(capacity_ - 1),
      data_(capacity_, 0.0F) {}

std::size_t RingBuffer::write(const float* samples, std::size_t count) noexcept {
    // relaxed: this thread is the only writer, so it already knows the value.
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
    // acquire: pairs with the consumer's release, so slots it freed are visible.
    const std::size_t readIndex = readIndex_.load(std::memory_order_acquire);

    const std::size_t free = mask_ - ((writeIndex - readIndex) & mask_);
    // Whole frames only when the block does not fit. The tail stays in the
    // caller's buffer and goes in on the next attempt, which every writer here
    // already loops for; committing it would leave the ring holding a fraction
    // of a frame for the consumer to trip over.
    const std::size_t take =
        wholeFrames(std::min(count, free), count,
                    frameSize_.load(std::memory_order_relaxed));
    if (take == 0) {
        return 0;
    }

    const std::size_t start = writeIndex & mask_;
    const std::size_t first = std::min(take, capacity_ - start);

    std::memcpy(data_.data() + start, samples, first * sizeof(float));
    if (take > first) {
        std::memcpy(data_.data(), samples + first, (take - first) * sizeof(float));
    }

    // release: publishes the samples above before the index that exposes them.
    writeIndex_.store((writeIndex + take) & mask_, std::memory_order_release);
    return take;
}

std::size_t RingBuffer::read(float* out, std::size_t count) noexcept {
    std::size_t       readIndex  = readIndex_.load(std::memory_order_relaxed);
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_acquire);

    // Honoured here rather than by the producer because the read index belongs
    // to this thread. Three atomic operations and no branch that allocates or
    // blocks, so the callback stays real-time safe.
    if (flushRequested_.load(std::memory_order_acquire)) {
        readIndex = writeIndex;
        readIndex_.store(readIndex, std::memory_order_release);
        flushRequested_.store(false, std::memory_order_release);
        return 0;
    }

    const std::size_t available = (writeIndex - readIndex) & mask_;
    // And whole frames only on the way out, for the same reason from the other
    // side: an underrun that consumed the first sample of a frame would hand the
    // second one to the next callback as if it were the first, swapping the
    // channels for the rest of the stream. The odd sample waits here instead.
    const std::size_t take =
        wholeFrames(std::min(count, available), count,
                    frameSize_.load(std::memory_order_relaxed));
    if (take == 0) {
        return 0;
    }

    const std::size_t start = readIndex & mask_;
    const std::size_t first = std::min(take, capacity_ - start);

    std::memcpy(out, data_.data() + start, first * sizeof(float));
    if (take > first) {
        std::memcpy(out + first, data_.data(), (take - first) * sizeof(float));
    }

    readIndex_.store((readIndex + take) & mask_, std::memory_order_release);
    return take;
}

std::size_t RingBuffer::availableToRead() const noexcept {
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_acquire);
    const std::size_t readIndex  = readIndex_.load(std::memory_order_acquire);
    return (writeIndex - readIndex) & mask_;
}

std::size_t RingBuffer::availableToWrite() const noexcept {
    return mask_ - availableToRead();
}

void RingBuffer::clear() noexcept {
    readIndex_.store(writeIndex_.load(std::memory_order_acquire),
                     std::memory_order_release);
    // A stop must not leave a request outstanding, or the next play() would sit
    // waiting for a consumer that has nothing left to acknowledge.
    flushRequested_.store(false, std::memory_order_release);
}

void RingBuffer::requestFlush() noexcept {
    flushRequested_.store(true, std::memory_order_release);
}

void RingBuffer::setFrameSize(std::size_t samplesPerFrame) noexcept {
    frameSize_.store(std::max<std::size_t>(samplesPerFrame, 1),
                     std::memory_order_release);
}

std::size_t RingBuffer::frameSize() const noexcept {
    return frameSize_.load(std::memory_order_acquire);
}

bool RingBuffer::flushPending() const noexcept {
    return flushRequested_.load(std::memory_order_acquire);
}

}  // namespace xpcog
