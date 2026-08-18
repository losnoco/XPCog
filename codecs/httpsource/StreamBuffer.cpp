#include "StreamBuffer.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace xpcog::codecs {
namespace {

/// Half the ring is kept clear of the producer so the other half always holds
/// recent history, which is what makes a short backward seek free. Cog does the
/// same with `maxBuffered = bufferSize / 2`.
[[nodiscard]] std::size_t readAhead(std::size_t capacity) noexcept {
    return capacity / 2;
}

}  // namespace

StreamBuffer::StreamBuffer(std::size_t capacity)
    : ring_(capacity), mask_(static_cast<std::int64_t>(capacity) - 1) {
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0 &&
           "StreamBuffer capacity must be a power of two");
}

std::size_t StreamBuffer::writable() const noexcept {
    const std::size_t limit = readAhead(ring_.size());
    return (remaining_ >= limit) ? 0 : limit - remaining_;
}

std::size_t StreamBuffer::write(const std::byte* data, std::size_t bytes) {
    const std::size_t take = std::min(bytes, writable());
    if (take == 0) {
        return 0;
    }

    const auto        at    = static_cast<std::size_t>(resumeOffset() & mask_);
    const std::size_t first = std::min(take, ring_.size() - at);
    std::memcpy(ring_.data() + at, data, first);
    if (first < take) {
        std::memcpy(ring_.data(), data + first, take - first);
    }

    remaining_ += take;

    // Anything more than a ring behind the write head has been overwritten.
    floor_ = std::max(floor_, resumeOffset() - static_cast<std::int64_t>(ring_.size()));
    return take;
}

bool StreamBuffer::applyPendingSkip() noexcept {
    if (skipBytes_ <= 0) {
        return true;
    }

    const auto drop = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(remaining_), skipBytes_));
    pos_ += static_cast<std::int64_t>(drop);
    remaining_ -= drop;
    skipBytes_ -= static_cast<std::int64_t>(drop);
    return skipBytes_ == 0;
}

std::size_t StreamBuffer::readable() const noexcept {
    return (skipBytes_ > 0) ? 0 : remaining_;
}

std::size_t StreamBuffer::read(void* out, std::size_t bytes) {
    const std::size_t take = std::min(bytes, readable());
    if (take == 0) {
        return 0;
    }

    auto*             dst   = static_cast<std::byte*>(out);
    const auto        at    = static_cast<std::size_t>(pos_ & mask_);
    const std::size_t first = std::min(take, ring_.size() - at);
    std::memcpy(dst, ring_.data() + at, first);
    if (first < take) {
        std::memcpy(dst + first, ring_.data(), take - first);
    }

    pos_ += static_cast<std::int64_t>(take);
    remaining_ -= take;
    return take;
}

bool StreamBuffer::seekWithinWindow(std::int64_t target) noexcept {
    if (target < 0) {
        return false;
    }

    if (target == pos_) {
        skipBytes_ = 0;
        return true;
    }

    const auto capacity = static_cast<std::int64_t>(ring_.size());

    if (target > pos_ && target - pos_ < capacity) {
        // Forward, close enough that waiting beats reconnecting. The data may
        // not have arrived; read() discards it as it does.
        skipBytes_ = target - pos_;
        return true;
    }

    // Backward, into history the producer has not yet overwritten. The bytes
    // behind the cursor are still in the ring -- rewinding the cursor is all it
    // takes to expose them again.
    const std::int64_t back    = pos_ - target;
    const auto         history = capacity - static_cast<std::int64_t>(remaining_);
    if (back > 0 && back <= history && target >= floor_) {
        skipBytes_ = 0;
        remaining_ += static_cast<std::size_t>(back);
        pos_ = target;
        return true;
    }

    return false;
}

void StreamBuffer::reset(std::int64_t position) noexcept {
    pos_       = position;
    remaining_ = 0;
    skipBytes_ = 0;
    // The ring still holds the previous request's bytes; none of them describe
    // the stream at this offset any more.
    floor_ = position;
}

}  // namespace xpcog::codecs
