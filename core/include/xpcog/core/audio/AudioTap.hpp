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
//
// ---------------------------------------------------------------------------
// Why there are two ways to read
// ---------------------------------------------------------------------------
// readLatest() -- "the newest N samples" -- is the obvious one and is wrong for
// anything that redraws on a clock. It ties the display's motion to the *writer's*
// rhythm: a device period of 4096 frames at 44.1 kHz lands every 93 ms, so five or
// six consecutive repaints see byte-identical audio and the sixth jumps a tenth of
// a second. The display is running at 60 Hz and moving at 11 Hz, which is what
// "choppy" means here. Small periods hide it; nothing fixes it, because the display
// has no cursor of its own to advance.
//
// readEnding() gives it one. The caller names the absolute frame the window should
// end at, and TapCursor (below) advances that number by wall-clock time instead of
// by writer activity -- so the display steps by its own frame interval whatever
// size the chunks arriving here are.

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
    /// the spectrum's 4096 so a slow repaint still finds a complete window, and so
    /// there is room behind the write head for a paced reader to sit in.
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
    ///
    /// For a display on a clock, prefer TapCursor: see the note above about what
    /// following the write head does to motion when the chunks are large.
    bool readLatest(float* out, std::size_t count) const noexcept;

    /// Copies the `count` mono samples ending at absolute frame `end`, oldest first.
    ///
    /// `end` is counted in the same units as framesWritten(): frames since the tap
    /// was created or last cleared. It is exclusive -- the sample at `end - 1` is
    /// the last one written to `out`.
    ///
    /// Zero-pads the front when the track has not yet produced `count` frames, as
    /// readLatest() does. Returns false when the request cannot be satisfied at
    /// all: past the write head (audio that has not happened yet) or so far behind
    /// it that the samples have been overwritten. A caller that gets false should
    /// keep its last frame on screen rather than draw silence.
    bool readEnding(std::uint64_t end, float* out, std::size_t count) const noexcept;

    /// Forgets everything. For a stop or a track change, so the display does not
    /// hold the previous track's tail.
    void clear() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Roughly how many frames arrive per write, which is how far a reader has to
    /// sit behind the head to have something to advance through between chunks.
    ///
    /// A decaying maximum rather than the last write or a true one: the last write
    /// is whatever a short final buffer happened to be, and a true maximum never
    /// comes back down after a single hiccup or a device reconfiguration. This
    /// tracks upward instantly -- a reader starved by a large chunk needs to know
    /// about it on the next frame, not eventually -- and falls back over a couple
    /// of hundred writes, which at any real period is a few seconds.
    [[nodiscard]] std::size_t writeGranularity() const noexcept {
        return granularity_.load(std::memory_order_relaxed);
    }

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
    std::atomic<std::size_t>        granularity_{0};
};

/// A display's own read position in a tap, advanced by the clock rather than by
/// the audio path.
///
/// The problem it exists for is in AudioTap's header comment: a visualiser that
/// asks for "the newest window" every repaint inherits the writer's rhythm, and a
/// playback chain pushing 93 ms chunks makes a 60 Hz display move eleven times a
/// second. This holds an absolute frame number, adds `elapsed * sampleRate` to it
/// each frame, and hands that to AudioTap::readEnding() -- so the window slides by
/// one frame interval per frame regardless of how the audio arrived.
///
/// The price is a fixed lag of one chunk. It has to trail the write head by about
/// that much, or there is nothing between the cursor and the head to advance
/// through and it stalls at the head exactly as before; the runway a chunk buys is
/// consumed in exactly the time until the next chunk arrives. When the chunks are
/// small the lag is small with them, so this costs nothing in the case where there
/// was nothing wrong.
///
/// Not thread-safe, and not meant to be: one of these belongs to one display, on
/// whichever thread draws it. Several displays may hold separate cursors into the
/// same tap -- nothing here writes to the tap.
class TapCursor {
public:
    /// The rate the tap is being filled at, which is what turns elapsed seconds
    /// into frames. Until this is set -- or if it is set to zero -- the cursor
    /// simply follows the write head, which is readLatest()'s behaviour.
    ///
    /// Changing it resyncs, because a cursor measured in frames means something
    /// different at a different rate.
    void setSampleRate(double rate) noexcept;

    /// Drops the position, so the next read() starts from the head again. For a
    /// stop, a seek, or a display that has been hidden and shown -- in each case
    /// the elapsed time since the last read describes nothing that was drawn.
    void reset() noexcept;

    /// Advances by `elapsedSeconds` of wall clock and fills `out` with the window
    /// ending there. Returns what AudioTap::readEnding() returned: false means
    /// nothing has played yet, or the cursor lost a race with the writer, and in
    /// both cases the caller should leave the last frame up.
    ///
    /// Resyncs itself when the cursor runs past the write head (the producer
    /// stalled, or the clock ran fast) or falls further behind than the tap keeps
    /// (the producer is faster than real time, as an offline render is, or the
    /// display missed a long stretch of frames). Both are a visible jump, and both
    /// are rare enough by construction to be worth less than the alternative,
    /// which is drifting silently out of sync with the audio.
    bool read(const AudioTap& tap, double elapsedSeconds, float* out,
              std::size_t count) noexcept;

    /// The frame the last window ended at. Exposed for tests.
    [[nodiscard]] std::uint64_t position() const noexcept { return cursor_; }

private:
    /// The longest gap between frames that is treated as real. A UI thread that
    /// was blocked for a second did not draw anything during it, and pretending it
    /// did just triggers the resync path with a bigger number.
    static constexpr double kMaxElapsedSeconds = 0.25;

    double        rate_   = 0.0;
    /// The fraction of a frame left over from the last advance. Kept because a
    /// display interval rarely divides a sample rate -- dropping it loses up to a
    /// frame per repaint, which at 60 Hz is a 2% drift and audible as a spectrum
    /// that slides steadily later.
    double        carry_  = 0.0;
    std::uint64_t cursor_ = 0;
    bool          synced_ = false;
};

}  // namespace xpcog
