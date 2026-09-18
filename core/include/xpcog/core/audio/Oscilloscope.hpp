// The arithmetic behind the oscilloscope panel, kept out of the panel so it can
// be tested without a window.
//
// No Cog counterpart: Cog has a spectrum and nothing else. Two questions have
// to be answered before a window of audio is drawn as a trace, and both are
// pure functions of the samples:
//
//  * Where the window should start. A display that shows "the newest N frames"
//    every repaint puts a steady tone at a different phase each time, so the
//    trace crawls across the screen at the beat between the tone and the frame
//    rate -- which is what an untriggered scope looks like. Starting each window
//    at a rising zero crossing instead holds the tone still, which is what a
//    trigger is.
//
//  * How to fit more samples than there are pixels. Drawing every sample as a
//    polyline vertex through a column narrower than a sample is aliasing: a
//    4 kHz tone across 800 px is 40 samples a pixel, and a line through every
//    fortieth is a shape the audio does not have. Folding each column to its
//    lowest and highest sample and drawing the band between them is what a
//    scope's phosphor did for free.

#pragma once

#include <cstddef>
#include <span>
#include <utility>

namespace xpcog {

/// The offset into `samples` at which a `window`-frame display should start
/// so a periodic signal holds still: the first rising zero crossing at or
/// after `samples.size() - 2 * window` that still leaves a whole window after
/// it. The search covers one window before the newest one, so a period up to a
/// window long is always found.
///
/// A crossing is a Schmitt trigger's: the signal has been below
/// -kTriggerHysteresis since the search began, and this is the first sample at
/// or above +kTriggerHysteresis after that. So dither, a fade's tail and the
/// noise floor of a quiet passage do not trigger on every sample near zero,
/// and a slow signal that takes several samples to cross the band still
/// triggers once, on the sample that leaves it.
///
/// Falls back to `samples.size() - window` -- the newest window, as an
/// untriggered display would show -- when there is no crossing: silence, a DC
/// offset, or a period longer than the search. And to zero when `samples` is
/// shorter than a window, which the caller should not have asked for.
[[nodiscard]] std::size_t triggerOffset(std::span<const float> samples,
                                        std::size_t            window) noexcept;

inline constexpr float kTriggerHysteresis = 0.002F;

/// Folds `samples` into `columns.size()` (low, high) pairs, one per pixel
/// column, each the extremes of the samples that column covers after `gain`
/// and clamping to [-1, 1]. With fewer samples than columns each column gets
/// the one sample nearest it, low and high alike, so the caller can draw a
/// polyline through either. Empty input leaves every column at (0, 0).
void foldForDisplay(std::span<const float> samples, float gain,
                    std::span<std::pair<float, float>> columns) noexcept;

}  // namespace xpcog
