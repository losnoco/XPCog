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

    /// Producer side. Asks the consumer to drop everything currently buffered,
    /// which is what a seek needs: without it, up to a ring's worth of audio
    /// from the old position keeps playing after the jump.
    ///
    /// The producer cannot do the dropping itself -- the read index belongs to
    /// the consumer, and moving it from another thread is exactly the race this
    /// class exists to avoid. So this only raises a flag, and the producer must
    /// then wait for flushPending() to clear before writing again. Writing
    /// sooner would push post-seek audio into the ring ahead of the discard,
    /// and the discard would throw it away too.
    void requestFlush() noexcept;

    /// True between requestFlush() and the consumer honouring it. Also false
    /// after clear(), so a stop does not strand a pending request.
    [[nodiscard]] bool flushPending() const noexcept;

private:
    const std::size_t  capacity_;  ///< power of two, one slot reserved
    const std::size_t  mask_;
    std::vector<float> data_;

    // Kept on separate cache lines: the producer writes one and the consumer the
    // other, every callback. Sharing a line would cause false sharing on the
    // audio path.
    alignas(64) std::atomic<std::size_t> writeIndex_{0};
    alignas(64) std::atomic<std::size_t> readIndex_{0};

    /// Set by the producer, cleared by the consumer. On its own cache line for
    /// the same reason as the indices.
    alignas(64) std::atomic<bool> flushRequested_{false};
};

}  // namespace xpcog
