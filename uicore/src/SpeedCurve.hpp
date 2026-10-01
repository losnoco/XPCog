#pragma once

// The pitch and tempo slider curve, shared by the two places that draw those
// sliders: the Pitch & Tempo preferences pane and the transport strip's speed
// popup. One copy, because two would be free to disagree about what a slider
// position means, and the position is what gets written to `pitch` and `tempo`
// -- a plist those two disagreed about would sound different depending on which
// control last touched it.

#include <algorithm>
#include <cmath>

namespace xpcog::app {

/// Cog's slider curve (PlaybackController.m speedScale): a position in 0..100
/// becomes a ratio in 0.2..5.0, quadratically, so the octave around 1.0 gets
/// most of the travel.
inline constexpr double kSpeedMin = 0.2;
inline constexpr double kSpeedMax = 5.0;

/// **Four times Cog's 100, and that is the one deliberate difference here.**
///
/// The curve is quadratic, so a step near 1.0 is worth far more than a step at
/// either end: with Cog's hundred positions the ratio moves about 0.039 per
/// step there, and the slider simply cannot express 0.98 or 0.99 -- it goes
/// 0.97, then 1.00, then 1.05. That is most of a semitone skipped in the range
/// people actually use.
///
/// 400 rather than more: it puts the step near 1.0 at about 0.0098, which is
/// what the two-decimal readout beside the slider can distinguish. Finer than
/// the label would be travel that changes nothing a reader can see.
///
/// The curve itself is untouched -- the same position, as a fraction of the
/// track, gives the same ratio it gives in Cog -- so an imported plist and this
/// slider still agree about what 1.5 means.
inline constexpr int kSpeedSliderMax = 400;

[[nodiscard]] inline double speedFromSlider(int position) {
    const double x = static_cast<double>(position) * (100.0 / kSpeedSliderMax);
    return ((x * x) * (kSpeedMax - kSpeedMin) / 10000.0) + kSpeedMin;
}

[[nodiscard]] inline int sliderFromSpeed(double ratio) {
    const double clamped = std::clamp(ratio, kSpeedMin, kSpeedMax);
    const double x =
        std::sqrt((clamped - kSpeedMin) * 10000.0 / (kSpeedMax - kSpeedMin));
    return static_cast<int>(std::lround(x * (kSpeedSliderMax / 100.0)));
}

/// Cog's snapSpeeds: the slider cannot otherwise land back on exactly 1.0 --
/// the nearest position is 0.997 -- and "very nearly unstretched" runs the
/// whole engine for nothing audible.
///
/// Half Cog's 0.01, and that follows from kSpeedSliderMax rather than being a
/// second opinion about it. At 0.01 it caught both positions either side of
/// 1.0, which made 1.01 unreachable -- the very complaint the finer slider is
/// here to fix, moved along by one step.
inline constexpr double kSpeedSnap = 0.005;

[[nodiscard]] inline double snapSpeed(double ratio) {
    return (std::fabs(ratio - 1.0) < kSpeedSnap) ? 1.0 : ratio;
}

}  // namespace xpcog::app
