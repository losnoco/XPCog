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
    const std::size_t take = std::min(count, free);
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
    const std::size_t readIndex  = readIndex_.load(std::memory_order_relaxed);
    const std::size_t writeIndex = writeIndex_.load(std::memory_order_acquire);

    const std::size_t available = (writeIndex - readIndex) & mask_;
    const std::size_t take      = std::min(count, available);
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
}

}  // namespace xpcog
