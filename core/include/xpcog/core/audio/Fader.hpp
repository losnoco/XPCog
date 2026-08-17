// The transport fade. Port of Cog Audio/Chain/DSP/DSPFaderNode.m plus the
// fadeAudio() kernel and FadedBuffer in FadedBuffer.m.
//
// A linear 200 ms crossfade -- Cog's `fadeTimeMS`, and its ramp is linear too
// (`vDSP_vrampmuladd`), so this keeps both the shape and the duration. Its job is
// that a seek neither ends nor begins with a step discontinuity, which is heard
// as a click at exactly the moment the user is listening for the jump to land.
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
// "To within the fill level" is doing real work there, so precisely: the tail
// spans [lastEmitted - 200 ms, lastEmitted], while the audio after the splice
// point spans [lastAudible, lastEmitted] -- and the two differ by however much of
// the shallow ring was unplayed, so up to (200 ms - fill) of the tail is audio
// the listener already heard, replayed. In steady playback the pump keeps that
// ring full (~186 ms), leaving ~14 ms of replay, which is inaudible. Cog escapes
// the approximation by capturing the exact flushed chunks; buying that here would
// mean the rings handing discarded audio back to the chain, which is a lot of
// machinery for 14 ms.
//
// The cost of that is one property this class used to have. To keep the rolling
// copy fed it must see every block, so active() stays true whenever fading is
// enabled rather than going false once a ramp lands. process() still leaves the
// samples untouched when there is nothing to apply, so the output is bit-identical
// either way; what it no longer does is let the chain skip the stage entirely.

#pragma once

#include "xpcog/core/audio/DSPNode.hpp"

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
    std::size_t        tailFrames_ = 0;
    std::size_t        tailRead_   = 0;
    bool               crossfading_ = false;
};

}  // namespace xpcog
