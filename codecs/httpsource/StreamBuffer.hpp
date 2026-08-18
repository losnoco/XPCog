// The ring between the network thread and the decoder, and the seek arithmetic
// over it. Port of the buffer half of Cog Plugins/HTTPSource/HTTPSource.m.
//
// Deliberately not thread-safe and deliberately non-blocking: HttpSource owns
// the mutex and does the waiting. Cog interleaves the ring arithmetic with its
// NSLock and usleep loops, which makes the part most likely to be wrong -- where
// the read cursor lands after a seek -- reachable only through a live socket.
// Everything here can be driven from a test with no network and no threads.
//
// The model is one absolute byte position plus a window around it:
//
//     pos          absolute offset of the read cursor in the stream
//     remaining    bytes buffered ahead of pos, ready to read
//     skipBytes    bytes to discard before reading, from a forward seek into
//                  data that has not arrived yet
//
// The ring index is `pos & mask`, so nothing is ever moved. Bytes behind pos are
// not cleared, and that is what makes a short backward seek free: the cursor
// walks back into history that is still sitting in the ring.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xpcog::codecs {

class StreamBuffer {
public:
    /// `capacity` must be a power of two; the index arithmetic is a mask.
    explicit StreamBuffer(std::size_t capacity);

    [[nodiscard]] std::size_t capacity() const noexcept { return ring_.size(); }

    // --- producer -----------------------------------------------------------

    /// Free space, honouring the reserve that keeps history available for a
    /// backward seek. Zero means the consumer has to drain before more fits.
    [[nodiscard]] std::size_t writable() const noexcept;

    /// Stores up to `writable()` bytes and returns how many. Never blocks.
    std::size_t write(const std::byte* data, std::size_t bytes);

    // --- consumer -----------------------------------------------------------

    /// Bytes available to hand to the decoder right now, after any pending skip.
    /// Zero while a forward seek is still waiting for its data to arrive.
    [[nodiscard]] std::size_t readable() const noexcept;

    /// Copies up to `readable()` bytes out and advances the cursor.
    std::size_t read(void* out, std::size_t bytes);

    /// Discards as much of a pending forward seek as is already buffered.
    /// Returns true when the skip is exhausted and reading can resume.
    bool applyPendingSkip() noexcept;

    // --- position -----------------------------------------------------------

    /// Where the decoder believes it is: the cursor plus any unconsumed skip.
    [[nodiscard]] std::int64_t tell() const noexcept { return pos_ + skipBytes_; }

    [[nodiscard]] std::int64_t pos() const noexcept { return pos_; }
    [[nodiscard]] std::size_t  buffered() const noexcept { return remaining_; }
    [[nodiscard]] std::int64_t pendingSkip() const noexcept { return skipBytes_; }

    /// The absolute offset a reconnect should resume from -- past everything
    /// already buffered, so a Range request does not fetch it twice.
    [[nodiscard]] std::int64_t resumeOffset() const noexcept {
        return pos_ + static_cast<std::int64_t>(remaining_);
    }

    /// Oldest offset whose bytes are still trustworthy: nothing before this has
    /// either been written yet or survived being overwritten.
    [[nodiscard]] std::int64_t oldestValid() const noexcept { return floor_; }

    /// Moves the cursor to `target` using only what the window already holds, or
    /// what a forward seek can wait for. False means the caller must reconnect.
    ///
    /// Three cases, from Cog:
    ///   * already there -- cancel any pending skip and do nothing;
    ///   * forward, within one buffer -- record a skip and let read() discard it
    ///     as it arrives, which costs nothing and covers a decoder probing ahead;
    ///   * backward, within the history still in the ring -- rewind the cursor
    ///     and grow the readable span, since those bytes were never overwritten.
    ///
    /// The backward case additionally checks oldestValid(), which Cog does not.
    /// Cog's test is purely `pos - target <= bufferSize - remaining`, a statement
    /// about ring geometry that says nothing about whether those bytes were ever
    /// written. After a reconnect -- which restarts the cursor at a new offset
    /// while leaving the ring full of the previous request's bytes -- that test
    /// passes and hands the decoder stale audio.
    [[nodiscard]] bool seekWithinWindow(std::int64_t target) noexcept;

    /// Drops everything and restarts the cursor at `position`, for a reconnect
    /// that cannot be served from the window.
    void reset(std::int64_t position) noexcept;

private:
    std::vector<std::byte> ring_;
    std::int64_t           mask_ = 0;

    std::int64_t pos_       = 0;
    std::size_t  remaining_ = 0;
    std::int64_t skipBytes_ = 0;
    std::int64_t floor_     = 0;
};

}  // namespace xpcog::codecs
