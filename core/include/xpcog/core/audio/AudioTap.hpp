// A window onto the audio that is being played, for the visualiser to look at.
//
// Not a RingBuffer. That one is a queue: what the consumer reads, the producer may
// reuse, and every sample is consumed exactly once. A visualiser wants the opposite
// -- the most recent N samples, over and over, whether or not it read them last
// time, and with no ability to hold the audio path up by falling behind. So this is
// a circular buffer that is always overwritten and never drained.
//
// Where it is filled matters more than how. Cog posts PCM into its own circular
// buffer from up in the chain (`-postVisPCM:amount:`), which leaves the drawing
// ahead of the sound by however deep the buffering is -- so `-copyVisPCM:` has to
// read *behind* the write cursor by the device latency plus a margin, and Cog
// carries that arithmetic in VisualizationController.m.
//
// XPCog fills it in the device callback instead, after the gain, which is the last
// place the audio exists before the driver takes it. That removes the compensation
// rather than reimplementing it: what is written is what is about to be heard, give
// or take the device's own period. That residual is one buffer -- ~10-20 ms -- and
// is deliberately not corrected, because a spectrum leading the music by a fiftieth
// of a second is not something an eye can see, and a latency estimate that is wrong
// in the other direction is.
//
// Real-time safety: write() allocates nothing, locks nothing, and touches only
// relaxed atomics. Per-sample atomic stores rather than a memcpy over plain floats,
// which sounds worse than it is -- a relaxed store of an aligned float is a plain
// store on every architecture this runs on, with no fence -- and buys the property
// that a reader racing the writer is defined behaviour rather than merely
// unobservable. Cog's equivalent is a bare float array with no synchronisation at
// all; the cost of doing it properly here was too low to justify copying that.
//
// The reader can still see a seam: the writer may lap it mid-copy, in which case one
// displayed frame contains samples from two different moments. That is accepted, not
// overlooked -- the alternative is making the audio path wait for a repaint, and a
// visualiser is never worth that. It shows up as a single frame of hash in a display
// that redraws sixty times a second.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xpcog {

class AudioTap {
public:
    /// `capacityFrames` is rounded up to a power of two, so the wrap is a mask.
    /// It should comfortably exceed the analysis window; the default is four times
    /// the spectrum's 4096 so a slow repaint still finds a complete window.
    explicit AudioTap(std::size_t capacityFrames = 1U << 14);

    /// Mixes `count` interleaved samples down to mono and appends them.
    /// Real-time safe. `channels` of zero is ignored rather than dividing by it.
    void write(const float* samples, std::size_t count, std::size_t channels) noexcept;

    /// Copies the newest `count` mono samples into `out`, oldest first.
    ///
    /// Zero-fills whatever has not been written yet, so a display can start drawing
    /// immediately after playback begins instead of waiting for a full window --
    /// which is what Cog's bzero of the short read does too.
    ///
    /// Returns false when nothing at all has been written, so a caller can tell
    /// "silence" from "not started" and draw nothing rather than a flat line.
    bool readLatest(float* out, std::size_t count) const noexcept;

    /// Forgets everything. For a stop or a track change, so the display does not
    /// hold the previous track's tail.
    void clear() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Total mono frames ever written. Monotonic across clear() only in the sense
    /// that clear() resets it; exposed for tests.
    [[nodiscard]] std::uint64_t framesWritten() const noexcept {
        return written_.load(std::memory_order_acquire);
    }

private:
    std::size_t                     capacity_;
    std::size_t                     mask_;
    std::vector<std::atomic<float>> samples_;
    std::atomic<std::uint64_t>      written_{0};
};

}  // namespace xpcog
