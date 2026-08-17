// The 31-band graphic equaliser. Port of Cog Audio/Chain/DSP/DSPEqualizerNode.m.
//
// Cog runs the whole thing through vDSP_biquadm, Accelerate's multichannel
// biquad cascade: one setup object holding 31 sections across every channel,
// fed the coefficients as doubles and the audio as floats. There is no portable
// equivalent, so the cascade is written out here -- which PORTING.md flagged as
// M4's one kernel where numeric drift from losing vDSP would otherwise be
// silent.
//
// The band centres and Q are Cog's, unchanged: the same 31 frequencies Apple's
// AUGraphicEQ uses, and a fixed Q of 1.4 across all of them. The coefficient
// formulas are the standard RBJ peaking-EQ pair, normalised by a0, again
// matching Cog term for term -- the point of this file is a different
// *implementation* of the same filter, not a different filter.
//
// Two deliberate differences, both documented at the call site:
//
//   * State is double, where vDSP_biquadm keeps it single. A 20 Hz peaking
//     section at 44.1 kHz has its poles very close to the unit circle, which is
//     precisely where single-precision state accumulates the most error, and
//     double state costs nothing at 31 sections.
//   * A flat equaliser is skipped rather than run. Cog processes the cascade
//     regardless, so a 0 dB setting still pushes every sample through 31
//     sections; active() lets the chain leave the buffer alone, which is what
//     makes flat bit-transparent by construction instead of by luck.
//
// Bands at or above Nyquist become identity sections rather than being dropped,
// which is Cog's behaviour and keeps band indices stable across sample rates:
// at 32 kHz the 16 and 20 kHz sliders simply stop doing anything.

#pragma once

#include "xpcog/core/audio/DSPNode.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace xpcog {

class Equalizer final : public DSPNode {
public:
    static constexpr int kBands = 31;

    /// The band centre frequencies in Hz, low to high. kBands entries.
    [[nodiscard]] static std::span<const double> bandFrequencies() noexcept;

    /// The settings key holding each band's gain, in the same order.
    ///
    /// Kept beside the frequencies rather than wherever settings happen to be
    /// read, because the one thing that must never drift is which key belongs to
    /// which centre: reorder them and every existing user's curve silently
    /// shifts along the spectrum, which no test of the filter itself would
    /// notice. Both tables live in one file so they are edited together.
    [[nodiscard]] static std::span<const char* const> bandSettingsKeys() noexcept;

    /// The fixed Q every band uses, as in Cog.
    static constexpr double kQ = 1.4;

    Equalizer();

    /// Overall gain in dB applied ahead of the bands, matching Cog's eqPreamp.
    /// Exists because boosting bands without headroom clips, and the preamp is
    /// how a user buys that headroom back.
    void setPreamp(double decibels);
    [[nodiscard]] double preamp() const noexcept { return preampDb_; }

    /// Gain in dB for one band. Out-of-range indices are ignored rather than
    /// asserted: these come from settings, and a stored file with the wrong
    /// number of bands should not be fatal.
    void setBandGain(int band, double decibels);
    [[nodiscard]] double bandGain(int band) const noexcept;

    /// Sets as many bands as `decibels` holds, up to kBands.
    void setBandGains(std::span<const double> decibels);

    void                       prepare(const AudioFormat& format) override;
    void                       process(float* samples, std::size_t frames) override;
    void                       reset() override;
    [[nodiscard]] bool         active() const override;

    /// One band's biquad at the prepared sample rate, normalised so a0 is 1.
    ///
    /// Exposed because the curve is worth plotting, and because it lets a test
    /// evaluate the cascade's transfer function directly and compare that
    /// against what process() actually does to a sine -- two independent
    /// computations of the same filter, which is the only way to catch a
    /// transposed-form slip that still sounds plausible.
    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };
    [[nodiscard]] Biquad coefficients(int band) const;

private:
    void rebuild();

    /// Per band, then per channel: the two-element state of a transposed
    /// direct-form II section.
    struct State {
        double s1 = 0.0;
        double s2 = 0.0;
    };

    double              preampDb_ = 0.0;
    double              preampGain_ = 1.0;
    std::vector<double> gainsDb_;
    std::vector<Biquad> biquads_;
    std::vector<State>  state_;
    double              sampleRate_ = 0.0;
    int                 channels_   = 0;
};

}  // namespace xpcog
