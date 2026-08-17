// Single-producer / single-consumer lock-free ring of float samples.
//
// This is what keeps the audio callback real-time safe. Cog's callback
// (Audio/Output/OutputCoreAudio.m:877) takes an NSLock and enters an
// @autoreleasepool on the RT thread, allocating AudioChunks as it goes. XPCog
// deliberately does not: the feeder thread pulls through the node graph and
// writes here, and the callback only reads.
//
// Exactly one producer thread and one consumer thread. Any other arrangement is
// undefined -- there is no CAS loop, only acquire/release pairing on two indices.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace xpcog {

class RingBuffer {
public:
    /// `capacity` is rounded up to a power of two so index wrapping is a mask.
    /// One slot is reserved to disambiguate full from empty, so usable capacity
    /// is one less than the allocation.
    explicit RingBuffer(std::size_t capacity);

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /// Producer side. Writes as many of `count` samples as fit; returns how many
    /// were taken. Never blocks.
    std::size_t write(const float* samples, std::size_t count) noexcept;

    /// Consumer side. Reads up to `count` samples; returns how many were
    /// delivered. Never blocks. Safe to call from a real-time callback: no locks,
    /// no allocation, no system calls.
    std::size_t read(float* out, std::size_t count) noexcept;

    [[nodiscard]] std::size_t availableToRead() const noexcept;
    [[nodiscard]] std::size_t availableToWrite() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_ - 1; }

    /// Consumer side only, and only while the producer is known to be stopped
    /// (during a device reconfigure).
    void clear() noexcept;

private:
    const std::size_t  capacity_;  ///< power of two, one slot reserved
    const std::size_t  mask_;
    std::vector<float> data_;

    // Kept on separate cache lines: the producer writes one and the consumer the
    // other, every callback. Sharing a line would cause false sharing on the
    // audio path.
    alignas(64) std::atomic<std::size_t> writeIndex_{0};
    alignas(64) std::atomic<std::size_t> readIndex_{0};
};

}  // namespace xpcog
