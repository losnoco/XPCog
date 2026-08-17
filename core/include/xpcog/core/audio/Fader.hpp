// The transport fade. Port of Cog Audio/Chain/DSP/DSPFaderNode.m plus the
// fadeAudio() kernel and FadedBuffer in FadedBuffer.m.
//
// A 200 ms crossfade -- Cog's `fadeTimeMS`. Its job is that a seek neither ends
// nor begins with a step discontinuity, which is heard as a click at exactly the
// moment the user is listening for the jump to land.
//
// The curve departs from Cog's, which ramps linearly (`vDSP_vrampmuladd`). A seek
// lands somewhere unrelated, so the two sides of the crossfade are uncorrelated
// and their *powers* add: linear complements of 0.5 each leave half the power, an
// audible 3 dB dip through the middle of every seek. Sine and cosine legs hold the
// sum of squares at one, which is the right curve for uncorrelated material.
//
// Driven by reset() rather than by its own transport API. reset() already means
// "the signal does not flow across this point" -- it is called at track start and
// after a seek, and it is what tells the equaliser to drop its filter state. A
// fader's correct response to that same event is to crossfade, so there is nothing
// extra for the engine to call and no way for a new discontinuity to be introduced
// without the fade following it.
//
// Fading *out* is the interesting half, because the audio to fade out has already
// been thrown away by the time this stage sees anything new. Cog solves it by
// retaining the discarded audio in a FadedBuffer and mixing its decaying tail over
// the new position. The same idea works here because this stage is last in the
// chain: everything it emits goes straight to the shallow ring, so a rolling copy
// of its last 200 ms of output is, to within the ring's fill level, the audio a
// seek is about to discard.
//
// Which part of that copy to use is the whole game. The history spans
// [lastEmitted - 200 ms, lastEmitted], but only its newest frames are still
// queued; the rest has already been heard. Starting the tail at its oldest frame
// therefore *replays* the difference, and a replayed 14 ms is a stutter rather
// than a fade -- audible, and reported as such.
//
// So the engine tells this stage how many frames the flush is discarding
// (noteDiscardedFrames, from the feeder, which is the only place that count can be
// read before the discard happens), and the tail is exactly that many frames taken
// from the newest end of the history: precisely the audio that was about to play
// and never did. Zero discarded means nothing was queued -- track start -- and the
// fade is a plain ramp in with no tail at all.
//
// The ramp then spans the tail rather than the nominal fade time, so both sides
// finish together; a tail shorter than the fade would otherwise stop dead at
// whatever level it had reached, which is the step being avoided.
//
// The cost of that is one property this class used to have. To keep the rolling
// copy fed it must see every block, so active() stays true whenever fading is
// enabled rather than going false once a ramp lands. process() still leaves the
// samples untouched when there is nothing to apply, so the output is bit-identical
// either way; what it no longer does is let the chain skip the stage entirely.

#pragma once

#include "xpcog/core/audio/DSPNode.hpp"

#include <atomic>
#include <cstddef>
#include <vector>

namespace xpcog {

class Fader final : public DSPNode {
public:
    /// Cog's fadeTimeMS.
    static constexpr double kDefaultFadeMilliseconds = 200.0;

    /// Cog's `enableFading`. Off means this stage never touches the signal and
    /// keeps no history.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void setFadeMilliseconds(double milliseconds);

    /// Where the ramp currently sits, for tests and for anyone chasing a click.
    [[nodiscard]] double level() const noexcept { return level_; }

    /// True while the outgoing tail is still being mixed out.
    [[nodiscard]] bool crossfading() const noexcept { return crossfading_; }

    /// How many frames of already-emitted audio the next reset() is discarding,
    /// which is what its fade out has to consist of. Called by the feeder just
    /// before it asks for the discard, because that is the last moment the count
    /// is still readable.
    ///
    /// Consumed by that reset() and then cleared, so a reset with no preceding
    /// note -- a track start rather than a seek -- correctly fades in from
    /// silence instead of mixing out a tail nothing is waiting to hear.
    ///
    /// Atomic because the two are not the same thread: only the feeder can read
    /// the ring's fill before it asks for the discard, while reset() belongs to
    /// the pump. Published before the flush epoch the pump waits on, so the value
    /// is visible by the time it acts.
    void noteDiscardedFrames(std::size_t frames);

    void prepare(const AudioFormat& format) override;
    void process(float* samples, std::size_t frames) override;

    /// Starts a crossfade: the retained tail mixes out as the new audio mixes in.
    /// With no tail yet -- at track start -- it is a plain fade in. Snaps to unity
    /// when disabled.
    void reset() override;

    [[nodiscard]] bool active() const override;

private:
    void   rebuildHistory();
    void   captureFrame(const float* frame);
    [[nodiscard]] std::size_t fadeFrames() const;

    bool   enabled_      = false;
    double milliseconds_ = kDefaultFadeMilliseconds;
    double sampleRate_   = 0.0;
    int    channels_     = 0;

    double level_  = 1.0;
    double target_ = 1.0;
    double step_   = 0.0;

    /// A rolling copy of recent output, oldest to newest once wrapped, which is
    /// the audio a flush discards.
    std::vector<float> history_;
    std::size_t        historyWrite_  = 0;
    std::size_t        historyFilled_ = 0;

    /// The tail captured at reset(), in chronological order, consumed as it mixes
    /// out. Held separately from history_ so the crossfade's own output can go on
    /// filling the history without overwriting what it is still reading.
    std::vector<float> tail_;
    std::size_t        tailFrames_      = 0;
    std::size_t        tailRead_        = 0;
    std::atomic<std::size_t> pendingDiscarded_{0};
    bool               crossfading_     = false;
};

}  // namespace xpcog
