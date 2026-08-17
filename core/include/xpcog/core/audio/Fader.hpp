// The transport fade. Port of Cog Audio/Chain/DSP/DSPFaderNode.m and the
// fadeAudio() kernel in FadedBuffer.m.
//
// A linear gain ramp over 200 ms -- Cog's `fadeTimeMS`, and its ramp is linear
// too (`vDSP_vrampmuladd`), so this keeps both the shape and the duration. Its
// job is that a seek does not begin with a step discontinuity, which is heard as
// a click at exactly the moment the user is listening for the jump to land.
//
// Driven by reset() rather than by its own transport API, which is worth
// explaining. reset() already means "the signal does not flow across this point"
// -- it is called at track start and after a seek, and it is what tells the
// equaliser to drop its filter state. A fader's correct response to that same
// event is to ramp in from silence, so there is nothing extra for the engine to
// call and no way for a new discontinuity to be introduced without the fade
// following it.
//
// **Only the fade in is implemented.** Cog also fades the *outgoing* audio out
// across a seek, which it can do because DSPFaderNode retains the discarded
// audio in a FadedBuffer and mixes its decaying tail over the new position. Here
// the pre-seek audio is discarded by a ring flush and is gone before this stage
// would see it again, so a symmetric crossfade needs this node to retain a tail
// of its own output. That is a real gap, not an oversight: it means a seek ramps
// in cleanly but still cuts the outgoing audio abruptly.
//
// Bit-transparency: with fading disabled, or once a ramp has landed at unity,
// active() reports false and the chain skips the stage entirely, so the samples
// are untouched rather than multiplied by 1.0f.

#pragma once

#include "xpcog/core/audio/DSPNode.hpp"

#include <cstddef>

namespace xpcog {

class Fader final : public DSPNode {
public:
    /// Cog's fadeTimeMS.
    static constexpr double kDefaultFadeMilliseconds = 200.0;

    /// Cog's `enableFading`. Off means this stage never touches the signal.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    void setFadeMilliseconds(double milliseconds);

    /// Where the ramp currently sits, for tests and for anyone debugging a click.
    [[nodiscard]] double level() const noexcept { return level_; }

    void prepare(const AudioFormat& format) override;
    void process(float* samples, std::size_t frames) override;

    /// Starts a fade in from silence, or snaps to unity when disabled.
    void reset() override;

    [[nodiscard]] bool active() const override;

private:
    void beginFadeIn();

    bool   enabled_       = false;
    double milliseconds_  = kDefaultFadeMilliseconds;
    double sampleRate_    = 0.0;
    int    channels_      = 0;
    double level_         = 1.0;
    double target_        = 1.0;
    double step_          = 0.0;
};

}  // namespace xpcog
