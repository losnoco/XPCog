#include "xpcog/core/audio/Equalizer.hpp"

#include "xpcog/core/AudioFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <span>

namespace xpcog {
namespace {

/// Apple's AUGraphicEQ centres, which is what Cog uses.
constexpr double kFrequencies[Equalizer::kBands] = {
    20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,    100.0,   125.0,
    160.0,  200.0,  250.0,  315.0,  400.0,  500.0,  630.0,   800.0,   1000.0,
    1200.0, 1600.0, 2000.0, 2500.0, 3100.0, 4000.0, 5000.0,  6300.0,  8000.0,
    10000.0, 12000.0, 16000.0, 20000.0};

/// Cog's NSUserDefaults keys for the same bands, in the same order -- see
/// EqualizerWindowController's _cog_equalizer_band_settings(). `p` is Cog's
/// spelling of a decimal point.
constexpr const char* kSettingsKeys[Equalizer::kBands] = {
    "eq20Hz",   "eq25Hz",   "eq31p5Hz", "eq40Hz",   "eq50Hz",   "eq63Hz",
    "eq80Hz",   "eq100Hz",  "eq125Hz",  "eq160Hz",  "eq200Hz",  "eq250Hz",
    "eq315Hz",  "eq400Hz",  "eq500Hz",  "eq630Hz",  "eq800Hz",  "eq1kHz",
    "eq1p2kHz", "eq1p6kHz", "eq2kHz",   "eq2p5kHz", "eq3p1kHz", "eq4kHz",
    "eq5kHz",   "eq6p3kHz", "eq8kHz",   "eq10kHz",  "eq12kHz",  "eq16kHz",
    "eq20kHz"};

static_assert(std::size(kSettingsKeys) == std::size(kFrequencies),
              "every band needs exactly one settings key");

/// The RBJ peaking-EQ biquad, normalised by a0. Identical to Cog's
/// setupOneBand(), including the Nyquist guard that yields an identity section
/// rather than an out-of-band filter.
[[nodiscard]] Equalizer::Biquad peaking(double frequency, double gainDb, double q,
                                       double sampleRate) {
    if (frequency <= 0.0 || frequency >= sampleRate / 2.0) {
        return Equalizer::Biquad{};
    }

    const double a     = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * std::numbers::pi * frequency / sampleRate;
    const double sinW  = std::sin(omega);
    const double cosW  = std::cos(omega);
    const double alpha = sinW / (2.0 * q);

    const double b0 = 1.0 + (alpha * a);
    const double b1 = -2.0 * cosW;
    const double b2 = 1.0 - (alpha * a);
    const double a0 = 1.0 + (alpha / a);
    const double a1 = -2.0 * cosW;
    const double a2 = 1.0 - (alpha / a);

    return Equalizer::Biquad{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

}  // namespace

std::span<const double> Equalizer::bandFrequencies() noexcept {
    return std::span<const double>{kFrequencies, kBands};
}

std::span<const char* const> Equalizer::bandSettingsKeys() noexcept {
    return std::span<const char* const>{kSettingsKeys, kBands};
}

Equalizer::Equalizer() : gainsDb_(kBands, 0.0), biquads_(kBands) {}

void Equalizer::setEnabled(bool enabled) { enabled_ = enabled; }

void Equalizer::setPreamp(double decibels) {
    preampDb_   = decibels;
    preampGain_ = std::pow(10.0, decibels / 20.0);
}

void Equalizer::setBandGain(int band, double decibels) {
    if (band < 0 || band >= kBands) {
        return;
    }
    if (gainsDb_[static_cast<std::size_t>(band)] == decibels) {
        return;
    }
    gainsDb_[static_cast<std::size_t>(band)] = decibels;
    rebuild();
}

double Equalizer::bandGain(int band) const noexcept {
    if (band < 0 || band >= kBands) {
        return 0.0;
    }
    return gainsDb_[static_cast<std::size_t>(band)];
}

void Equalizer::setBandGains(std::span<const double> decibels) {
    const auto count = std::min<std::size_t>(decibels.size(), kBands);
    for (std::size_t band = 0; band < count; ++band) {
        gainsDb_[band] = decibels[band];
    }
    rebuild();
}

void Equalizer::prepare(const AudioFormat& format) {
    sampleRate_ = format.sampleRate;
    channels_   = static_cast<int>(format.channels);

    // Coefficients depend on the sample rate, so a format change rebuilds them.
    rebuild();

    // Sized here rather than in process(), which must not allocate. Every band
    // gets its own state per channel: sharing state across channels would make
    // one channel's history filter the other, collapsing the stereo image.
    state_.assign(static_cast<std::size_t>(kBands) * static_cast<std::size_t>(std::max(channels_, 0)),
                  State{});
}

void Equalizer::rebuild() {
    if (sampleRate_ <= 0.0) {
        return;
    }
    for (int band = 0; band < kBands; ++band) {
        biquads_[static_cast<std::size_t>(band)] =
            peaking(kFrequencies[band], gainsDb_[static_cast<std::size_t>(band)], kQ,
                    sampleRate_);
    }
}

Equalizer::Biquad Equalizer::coefficients(int band) const {
    if (band < 0 || band >= kBands) {
        return Biquad{};
    }
    return biquads_[static_cast<std::size_t>(band)];
}

bool Equalizer::active() const {
    // Checked first, and it is the whole point of the switch: a disabled
    // equaliser is inactive however far from flat its bands are, which is what
    // makes bypassing one different from flattening it.
    if (!enabled_) {
        return false;
    }
    if (preampDb_ != 0.0) {
        return true;
    }
    return std::ranges::any_of(gainsDb_, [](double gain) { return gain != 0.0; });
}

void Equalizer::reset() {
    std::ranges::fill(state_, State{});
}

void Equalizer::process(float* samples, std::size_t frames) {
    if (frames == 0 || channels_ <= 0 || !active() || state_.empty()) {
        return;
    }

    const auto channels = static_cast<std::size_t>(channels_);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            double value = static_cast<double>(samples[(frame * channels) + channel]) *
                           preampGain_;

            // Transposed direct form II, section by section. DF2T is the form
            // vDSP_biquadm uses and the one that keeps its two state values in
            // the same units as the signal, which matters here because 31
            // sections in series are 31 chances for a badly scaled intermediate
            // to lose precision.
            for (int band = 0; band < kBands; ++band) {
                const Biquad& biquad = biquads_[static_cast<std::size_t>(band)];
                State& state =
                    state_[(static_cast<std::size_t>(band) * channels) + channel];

                const double input  = value;
                const double output = (biquad.b0 * input) + state.s1;
                state.s1 = (biquad.b1 * input) - (biquad.a1 * output) + state.s2;
                state.s2 = (biquad.b2 * input) - (biquad.a2 * output);
                value    = output;
            }

            samples[(frame * channels) + channel] = static_cast<float>(value);
        }
    }
}

}  // namespace xpcog
