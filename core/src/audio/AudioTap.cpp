#include "xpcog/core/audio/AudioTap.hpp"

#include <algorithm>
#include <cmath>

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

    // The chunk size a paced reader has to allow for. Only the audio path writes
    // here, so this is a plain read-modify-write of a relaxed atomic rather than a
    // compare-exchange loop: nothing else can be storing to it.
    if (frames > 0) {
        const std::size_t previous = granularity_.load(std::memory_order_relaxed);
        // Decay by a two-hundredth, and by at least one frame so a small value
        // still gets down to where it belongs rather than sticking.
        const std::size_t decayed =
            previous - std::min(previous, std::max<std::size_t>(previous / 200, 1));
        granularity_.store(std::max(frames, decayed), std::memory_order_relaxed);
    }

    // Published last, with release ordering, so a reader that sees this count also
    // sees the samples it counts. Without that the newest frames of every window
    // would be whatever the slots held previously -- which is the *oldest* audio in
    // the buffer, a second ago, and would read as a spectrum that stutters.
    written_.store(cursor, std::memory_order_release);
}

bool AudioTap::readLatest(float* out, std::size_t count) const noexcept {
    return readEnding(written_.load(std::memory_order_acquire), out, count);
}

bool AudioTap::readEnding(std::uint64_t end, float* out,
                          std::size_t count) const noexcept {
    if (out == nullptr || count == 0) {
        return false;
    }

    const std::uint64_t cursor = written_.load(std::memory_order_acquire);
    if (cursor == 0 || end == 0) {
        return false;  // nothing has played yet, or nothing is being asked for
    }
    if (end > cursor) {
        return false;  // audio that has not been produced yet
    }

    // Never reach further back than the buffer holds: past that the slots have been
    // overwritten with *newer* audio, so reading them would splice the future into
    // the start of the window.
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(end, capacity_));
    const std::size_t wanted = std::min(count, available);
    const std::size_t pad    = count - wanted;

    const std::uint64_t begin = end - wanted;
    if (cursor - begin > capacity_) {
        return false;  // the window has been overwritten since the caller chose it
    }

    // What is missing goes at the front, so the newest sample stays at the end where
    // the caller expects it.
    std::fill_n(out, pad, 0.0F);

    for (std::size_t index = 0; index < wanted; ++index) {
        out[pad + index] =
            samples_[static_cast<std::size_t>(begin + index) & mask_].load(
                std::memory_order_relaxed);
    }
    return true;
}

void AudioTap::clear() noexcept {
    written_.store(0, std::memory_order_release);
    granularity_.store(0, std::memory_order_relaxed);
    for (std::atomic<float>& sample : samples_) {
        sample.store(0.0F, std::memory_order_relaxed);
    }
}

void TapCursor::setSampleRate(double rate) noexcept {
    if (rate == rate_) {
        return;
    }
    rate_ = rate > 0.0 ? rate : 0.0;
    reset();
}

void TapCursor::reset() noexcept {
    cursor_ = 0;
    carry_  = 0.0;
    synced_ = false;
}

bool TapCursor::read(const AudioTap& tap, double elapsedSeconds, float* out,
                     std::size_t count) noexcept {
    if (out == nullptr || count == 0) {
        return false;
    }

    const std::uint64_t head = tap.framesWritten();
    if (head == 0) {
        // A stop clears the tap; treat that as the start of something new rather
        // than carrying a position from the previous track into it.
        reset();
        return false;
    }

    if (rate_ <= 0.0) {
        // Nobody has said how fast the tap is being filled, so there is no way to
        // turn seconds into frames. Follow the head, which is what a visualiser did
        // before this class existed -- choppy with large chunks, but never wrong.
        synced_ = false;
        return tap.readEnding(head, out, count);
    }

    // How much audio a frame of the display's clock is worth. Both the step the
    // cursor takes and the margin it keeps: a runway of exactly one chunk is
    // marginally stable -- the cursor reaches the head at the same moment the next
    // chunk is due, so a callback a millisecond late, or a repaint a millisecond
    // early, starves it. One frame's slack on top costs a sixtieth of a second of
    // lag and turns a resync every chunk into none.
    const double  frames = std::clamp(elapsedSeconds, 0.0, kMaxElapsedSeconds) * rate_;
    const auto    step   = static_cast<std::uint64_t>(frames);

    // How far behind the head to sit. One chunk plus that margin, because a chunk
    // is exactly the runway needed to keep moving until the next one lands --
    // less and the cursor catches the head between chunks and stalls there, which
    // is the behaviour this exists to remove. Bounded by what is left of the buffer
    // behind the window, since anything older than that has been overwritten.
    const std::uint64_t window = std::min<std::uint64_t>(count, tap.capacity());
    const std::uint64_t maxLag = tap.capacity() - window;
    const std::uint64_t lag =
        std::min<std::uint64_t>(tap.writeGranularity() + step, maxLag);
    // Never back to zero, which readEnding() reads as "there is nothing to show"
    // and would blank the display for the first chunk of every track -- the one
    // moment when the whole chunk *is* the history. One sample in, zero-padded, is
    // what a display got from readLatest() at that point too.
    //
    // A track's first chunk is also the one time the lag cannot be had: there is no
    // history to sit in, so the cursor starts as far back as the chunk allows and
    // resyncs once, early, when it runs out. After that it has a track's worth of
    // history behind it and settles.
    const std::uint64_t resync = head - std::min(head - 1, lag);

    if (!synced_) {
        cursor_ = resync;
        carry_  = 0.0;
        synced_ = true;
    } else {
        const double advance = frames + carry_;
        const double whole   = std::floor(advance);
        carry_               = advance - whole;
        cursor_ += static_cast<std::uint64_t>(whole);

        // Ahead of the head means the producer stalled or the clock ran fast;
        // further behind than the tap keeps means the producer is running faster
        // than real time, or this display missed a long stretch of frames. Either
        // way the position is no longer usable and the only honest thing is to jump
        // back to where the audio actually is.
        if (cursor_ > head || head - cursor_ > maxLag) {
            cursor_ = resync;
            carry_  = 0.0;
        }
    }

    return tap.readEnding(cursor_, out, count);
}

}  // namespace xpcog
