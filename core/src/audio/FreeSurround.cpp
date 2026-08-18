#include "xpcog/core/audio/FreeSurround.hpp"

#include "channelmaps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xpcog {
namespace {

// Upstream's, and a float on purpose. It reaches the window function and the
// LFE crossover, so widening it to M_PI would move the output.
constexpr float kPi      = 3.141592654F;
constexpr float kEpsilon = 0.000001F;

/// Narrowed to float and widened straight back.
///
/// That looks like a no-op and is not. Upstream's helpers all *return* float
/// from double arguments, so every value passing through them is rounded to
/// single precision before it re-enters double arithmetic -- on every bin of
/// every block, in the signal path. Writing the round trip out preserves the
/// rounding exactly while letting the callers stay in double, which is the only
/// way to keep -Wdouble-promotion honest: the promotions are deliberate, and a
/// warning that fires forty times on deliberate code stops being read.
double roundToFloat(double x) {
    return static_cast<double>(static_cast<float>(x));
}

// The helpers upstream declares as returning float. `sqr` is the one that
// matters most: `amplitude` squares each component through it, so both squares
// are rounded to float *before* they are added.
double sqr(double x) {
    return roundToFloat(x * x);
}

double minOf(double a, double b) {
    return roundToFloat(a < b ? a : b);
}

double maxOf(double a, double b) {
    return roundToFloat(a > b ? a : b);
}

double clamp(double x) {
    return maxOf(-1, minOf(1, x));
}

double sign(double x) {
    return roundToFloat(x < 0 ? -1 : (x > 0 ? 1 : 0));
}

double amplitude(const double* real, const double* imag, std::size_t index) {
    return std::sqrt(sqr(real[index]) + sqr(imag[index]));
}

double phaseOf(const double* real, const double* imag, std::size_t index) {
    return std::atan2(imag[index], real[index]);
}

void polar(double amp, double phase, double* real, double* imag, std::size_t index) {
    real[index] = amp * std::cos(phase);
    imag[index] = amp * std::sin(phase);
}

/// Distance to the edge of the soundfield square, along a given angle.
double edgeDistance(double angle) {
    return minOf(std::sqrt(1 + sqr(std::tan(angle))),
                 std::sqrt(1 + sqr(1 / std::tan(angle))));
}

/// Index into the allocation grid, leaving `x` as the fractional offset.
int mapToGrid(double& x) {
    const double gp = ((x + 1) * 0.5) * (grid_res - 1);
    const double i  = minOf(grid_res - 2, std::floor(gp));
    x               = gp - i;
    return static_cast<int>(i);
}

/// Amplitude and phase difference into an x/y position in the soundfield.
///
/// The polynomial is a fit, not a derivation, and is copied term for term.
void transformDecode(double a, double p, double& x, double& y) {
    x = clamp(1.0047 * a + 0.46804 * a * p * p * p - 0.2042 * a * p * p * p * p +
              0.0080586 * a * p * p * p * p * p * p * p -
              0.0001526 * a * p * p * p * p * p * p * p * p * p * p -
              0.073512 * a * a * a * p - 0.2499 * a * a * a * p * p * p * p +
              0.016932 * a * a * a * p * p * p * p * p * p * p -
              0.00027707 * a * a * a * p * p * p * p * p * p * p * p * p * p +
              0.048105 * a * a * a * a * a * p * p * p * p * p * p * p -
              0.0065947 * a * a * a * a * a * p * p * p * p * p * p * p * p * p * p +
              0.0016006 * a * a * a * a * a * p * p * p * p * p * p * p * p * p * p * p -
              0.0071132 * a * a * a * a * a * a * a * p * p * p * p * p * p * p * p * p +
              0.0022336 * a * a * a * a * a * a * a * p * p * p * p * p * p * p * p * p * p * p -
              0.0004804 * a * a * a * a * a * a * a * p * p * p * p * p * p * p * p * p * p * p * p);
    y = clamp(0.98592 - 0.62237 * p + 0.077875 * p * p - 0.0026929 * p * p * p * p * p +
              0.4971 * a * a * p - 0.00032124 * a * a * p * p * p * p * p * p +
              9.2491e-006 * a * a * a * a * p * p * p * p * p * p * p * p * p * p +
              0.051549 * a * a * a * a * a * a * a * a +
              1.0727e-014 * a * a * a * a * a * a * a * a * a * a);
}

void transformCircularWrap(double& x, double& y, double refAngle) {
    if (refAngle == 90) {
        return;
    }
    refAngle = refAngle * static_cast<double>(kPi) / 180;
    // Single precision, because upstream's is: `90 * pi / 180` is int-times-float
    // and stays float until it is stored. Only reached when the wrap is moved off
    // 90 degrees, which Cog never does -- so this line is faithful but uncovered
    // by the golden fixture.
    const double baseAngle = static_cast<double>(90 * kPi / 180);
    double       angle      = std::atan2(x, y);
    double       length     = std::sqrt(x * x + y * y);
    length                  = length / edgeDistance(angle);
    if (std::abs(angle) < baseAngle / 2) {
        angle *= refAngle / baseAngle;
    } else {
        angle = static_cast<double>(kPi) -
                (-(((refAngle - static_cast<double>(2 * kPi)) *
                    (static_cast<double>(kPi) - std::abs(angle)) * sign(angle)) /
                   (static_cast<double>(2 * kPi) - baseAngle)));
    }
    length = length * edgeDistance(angle);
    x      = clamp(std::sin(angle) * length);
    y      = clamp(std::cos(angle) * length);
}

void transformFocus(double& x, double& y, double focus) {
    if (focus == 0) {
        return;
    }
    const double angle = std::atan2(x, y);
    double length = clamp(std::sqrt(x * x + y * y) / edgeDistance(angle));
    length = focus > 0 ? 1 - std::pow(1 - length, 1 + focus * 20)
                       : std::pow(length, 1 - focus * 20);
    length = length * edgeDistance(angle);
    x      = clamp(std::sin(angle) * length);
    y      = clamp(std::cos(angle) * length);
}

// vDSP's forward real transform returns twice the mathematical DFT that RealFft
// implements. Cog's kernel never divides that out, so the factor is baked into
// every amplitude the decoder works from -- and not only into the output level:
// the `ampL + ampR < epsilon` test below compares against it, so a bin that Cog
// treats as having a decodable position could otherwise be treated here as
// silence. Applied to the two amplitudes rather than to the total for exactly
// that reason.
//
// Measured against tests/golden/fsurround-5point1.f32 rather than taken from
// Apple's documentation, which is the only way to know what this build of vDSP
// actually did. Every block agreed on 4.0 for the output ratio to nine
// significant figures, which is 2 here and 1 for the inverse -- vDSP's inverse
// and RealFft's share a scaling. A constant ratio across all eight blocks is
// also the evidence that only the scale differed: a structural error does not
// come out as one number.
//
// Named here rather than folded into RealFft, which owes its own definition no
// apology. See RealFft.hpp.
constexpr double kVdspScale = 2.0;

}  // namespace

FreeSurround::FreeSurround(channel_setup setup, unsigned blockSize)
    : blockSize_(blockSize),
      channels_(static_cast<unsigned>(chn_alloc[setup].size())),
      setup_(setup),
      fft_(blockSize),
      window_(blockSize),
      left_(blockSize),
      right_(blockSize),
      timeDomain_(blockSize),
      timeDomainFloat_(blockSize),
      leftReal_(fft_.bins()),
      leftImag_(fft_.bins()),
      rightReal_(fft_.bins()),
      rightImag_(fft_.bins()),
      inputBuffer_(3 * static_cast<std::size_t>(blockSize)),
      outputBuffer_(static_cast<std::size_t>(blockSize + blockSize / 2) * channels_) {
    signalReal_.assign(channels_, std::vector<double>(fft_.bins()));
    signalImag_.assign(channels_, std::vector<double>(fft_.bins()));

    // sqrt of a Hann window over N, which carries the 1/N that RealFft's
    // unnormalised inverse leaves behind -- applied on analysis and again on
    // synthesis, half each time.
    for (unsigned k = 0; k < blockSize_; ++k) {
        // Single precision as far as the cosine, deliberately. Upstream's `pi` is
        // a float, so `2 * pi * k / N` never leaves single precision and the call
        // resolves to cosf. Evaluating it in double gives a subtly different
        // window -- and the window multiplies every sample on the way in and
        // again on the way out, so the difference is not academic.
        const float angle  = 2 * kPi * static_cast<float>(k) / static_cast<float>(blockSize_);
        const float shaped = 1.0F - std::cos(angle);
        window_[k] =
            std::sqrt(0.5 * static_cast<double>(shaped) / static_cast<double>(blockSize_));
    }

    setLowCutoff(40.0F / 22050.0F);
    setHighCutoff(90.0F / 22050.0F);
    flush();
}

void FreeSurround::setLowCutoff(float normalised) noexcept {
    lowCut_ = normalised * static_cast<float>(blockSize_ / 2);
}

void FreeSurround::setHighCutoff(float normalised) noexcept {
    highCut_ = normalised * static_cast<float>(blockSize_ / 2);
}

unsigned FreeSurround::buffered() const noexcept {
    return bufferEmpty_ ? 0 : blockSize_ / 2;
}

unsigned FreeSurround::channelCount(channel_setup setup) {
    return static_cast<unsigned>(chn_id[setup].size());
}

channel_id FreeSurround::channelAt(channel_setup setup, unsigned index) {
    return index < chn_id[setup].size() ? chn_id[setup][index] : ci_none;
}

void FreeSurround::flush() {
    std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0F);
    std::fill(inputBuffer_.begin(), inputBuffer_.end(), 0.0F);
    bufferEmpty_ = true;
}

const float* FreeSurround::decode(const float* input) {
    const std::size_t n = blockSize_;

    // The input buffer holds one and a half blocks of stereo: half a block of
    // history, then the block just handed in. Two overlapping transforms are
    // taken from it, half a block apart, which is what the sqrt-Hann window is
    // built to overlap-add cleanly.
    std::memcpy(&inputBuffer_[n], input, 2 * n * sizeof(float));
    decodeHalfBlock(&inputBuffer_[0]);
    decodeHalfBlock(&inputBuffer_[n]);
    std::memcpy(&inputBuffer_[0], &inputBuffer_[2 * n], n * sizeof(float));

    bufferEmpty_ = false;
    return outputBuffer_.data();
}

void FreeSurround::decodeHalfBlock(const float* input) {
    const std::size_t n    = blockSize_;
    const std::size_t half = n / 2;
    const unsigned    c    = channels_;

    for (std::size_t k = 0; k < n; ++k) {
        left_[k]  = static_cast<double>(input[2 * k]) * window_[k];
        right_[k] = static_cast<double>(input[2 * k + 1]) * window_[k];
    }

    fft_.forward(left_.data(), leftReal_.data(), leftImag_.data());
    fft_.forward(right_.data(), rightReal_.data(), rightImag_.data());

    // DC and Nyquist are discarded rather than steered -- there is no meaningful
    // stereo position at either. Zeroing them is also what makes the difference
    // between RealFft's unpacked layout and vDSP's packed one irrelevant here:
    // the two disagree only about where Nyquist lives, and neither is read.
    for (unsigned channel = 0; channel < c; ++channel) {
        signalReal_[channel][0]    = 0;
        signalImag_[channel][0]    = 0;
        signalReal_[channel][half] = 0;
        signalImag_[channel][half] = 0;
    }
    std::fill(signalReal_[c - 1].begin(), signalReal_[c - 1].end(), 0.0);
    std::fill(signalImag_[c - 1].begin(), signalImag_[c - 1].end(), 0.0);

    for (std::size_t f = 1; f < half; ++f) {
        const double ampL   = amplitude(leftReal_.data(), leftImag_.data(), f) * kVdspScale;
        const double ampR   = amplitude(rightReal_.data(), rightImag_.data(), f) * kVdspScale;
        const double phaseL = phaseOf(leftReal_.data(), leftImag_.data(), f);
        const double phaseR = phaseOf(rightReal_.data(), rightImag_.data(), f);

        const double ampDiff =
            clamp((ampL + ampR < static_cast<double>(kEpsilon)) ? 0
                                                              : (ampR - ampL) / (ampR + ampL));
        double phaseDiff = std::abs(phaseL - phaseR);
        if (phaseDiff > static_cast<double>(kPi)) {
            phaseDiff = static_cast<double>(2 * kPi) - phaseDiff;
        }

        double x = 0.0;
        double y = 0.0;
        transformDecode(ampDiff, phaseDiff, x, y);
        transformCircularWrap(x, y, static_cast<double>(circularWrap_));
        y = clamp(y - static_cast<double>(shift_));
        y = clamp(1 - (1 - y) * static_cast<double>(depth_));
        transformFocus(x, y, static_cast<double>(focus_));
        x = clamp(x * (static_cast<double>(frontSeparation_) * (1 + y) / 2 +
                       static_cast<double>(rearSeparation_) * (1 - y) / 2));

        const double ampTotal = std::sqrt(ampL * ampL + ampR * ampR);
        const double phaseFor[] = {
            phaseL,
            std::atan2(leftImag_[f] + rightImag_[f], leftReal_[f] + rightReal_[f]),
            phaseR};

        const int p = mapToGrid(x);
        const int q = mapToGrid(y);

        for (unsigned channel = 0; channel < c - 1; ++channel) {
            // Bilinear interpolation into the channel's 21x21 gain surface.
            const std::vector<float*>& a = chn_alloc[setup_][channel];
            polar(ampTotal * ((1 - x) * (1 - y) * static_cast<double>(a[q][p]) +
                              x * (1 - y) * static_cast<double>(a[q][p + 1]) +
                              (1 - x) * y * static_cast<double>(a[q + 1][p]) +
                              x * y * static_cast<double>(a[q + 1][p + 1])),
                  phaseFor[1 + static_cast<int>(
                                   sign(static_cast<double>(chn_xsf[setup_][channel])))],
                  signalReal_[channel].data(), signalImag_[channel].data(), f);
        }

        if (useLfe_ && static_cast<float>(f) < highCut_) {
            // Single precision again, and again because upstream's is: `pi`,
            // `lo_cut` and `hi_cut` are all floats, so the crossover is computed
            // in float and the call resolves to cosf. Uncovered by the golden
            // fixture -- Cog hardcodes bass redirection off. See the note in
            // tools/fsurround-golden/README.md.
            const float position = kPi * (static_cast<float>(f) - lowCut_) / (highCut_ - lowCut_);
            const double lfeLevel = static_cast<float>(f) < lowCut_
                                        ? 1
                                        : 0.5 * static_cast<double>(1.0F + std::cos(position));
            polar(ampTotal, phaseFor[1], signalReal_[c - 1].data(),
                  signalImag_[c - 1].data(), f);
            signalReal_[c - 1][f] *= lfeLevel;
            signalImag_[c - 1][f] *= lfeLevel;
            for (unsigned channel = 0; channel < c - 1; ++channel) {
                signalReal_[channel][f] *= (1 - lfeLevel);
                signalImag_[channel][f] *= (1 - lfeLevel);
            }
        }
    }

    // Slide the overlap window down and clear the space the new half block will
    // be added into.
    std::memmove(outputBuffer_.data(), outputBuffer_.data() + c * half,
                 n * c * sizeof(float));
    std::memset(outputBuffer_.data() + (n * c), 0, c * half * sizeof(float));

    for (unsigned channel = 0; channel < c; ++channel) {
        fft_.inverse(signalReal_[channel].data(), signalImag_[channel].data(),
                     timeDomain_.data());
        for (std::size_t k = 0; k < n; ++k) {
            timeDomainFloat_[k] = static_cast<float>(timeDomain_[k] * window_[k]);
        }
        for (std::size_t k = 0; k < n; ++k) {
            outputBuffer_[(c * half) + channel + (k * c)] += timeDomainFloat_[k];
        }
    }
}

}  // namespace xpcog
