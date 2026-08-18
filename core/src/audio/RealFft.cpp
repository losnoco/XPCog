#include "xpcog/core/audio/RealFft.hpp"

#include <cassert>
#include <cmath>
#include <numbers>

namespace xpcog {
namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

}  // namespace

RealFft::RealFft(std::size_t size) : size_(size), half_(size / 2) {
    assert(size_ >= 4 && (size_ & (size_ - 1)) == 0);

    const std::size_t pairs = size_ / 4 + 1;
    cos_.resize(pairs);
    sin_.resize(pairs);
    for (std::size_t k = 0; k < pairs; ++k) {
        const double angle =
            -kTwoPi * static_cast<double>(k) / static_cast<double>(size_);
        cos_[k] = std::cos(angle);
        sin_[k] = std::sin(angle);
    }
}

void RealFft::forward(const double* input, double* real, double* imaginary) const {
    const std::size_t half = size_ / 2;

    // Pack: evens into the real part, odds into the imaginary. The transform of
    // this half-length complex signal contains both subsequences' spectra,
    // superimposed; the loop below separates them.
    for (std::size_t n = 0; n < half; ++n) {
        real[n]      = input[2 * n];
        imaginary[n] = input[2 * n + 1];
    }

    half_.forward(real, imaginary);

    // Bin 0 carries both DC and Nyquist, because the even and odd spectra are
    // both real there and add and subtract cleanly. Taken first, since the pair
    // loop below overwrites slot 0.
    const double zeroReal = real[0];
    const double zeroImag = imaginary[0];
    real[0]         = zeroReal + zeroImag;
    imaginary[0]    = 0.0;
    real[half]      = zeroReal - zeroImag;
    imaginary[half] = 0.0;

    // Walk pairs (k, half-k) inward. Both are computed from the same two input
    // slots, so taking them together is what makes this work in place.
    //
    // With E and O the spectra of the even and odd subsequences:
    //     E[k] = (Z[k] + conj(Z[half-k])) / 2
    //     O[k] = (Z[k] - conj(Z[half-k])) / 2i
    //     X[k]        = E[k] + W*O[k]
    //     X[half - k] = conj(E[k] - W*O[k])
    // where W = exp(-2*pi*i*k/N). The second line is the one worth checking
    // against the tests rather than against intuition: it is *not* conj(X[k]),
    // which is the mistake this shape invites.
    for (std::size_t k = 1; k <= size_ / 4; ++k) {
        const std::size_t mirror = half - k;

        const double kReal      = real[k];
        const double kImag      = imaginary[k];
        const double mirrorReal = real[mirror];
        const double mirrorImag = imaginary[mirror];

        const double evenReal = 0.5 * (kReal + mirrorReal);
        const double evenImag = 0.5 * (kImag - mirrorImag);

        // (Z[k] - conj(Z[half-k])) / 2, then multiplied by -i, which is the
        // rotation that turns a difference into the odd subsequence's spectrum.
        const double diffReal = 0.5 * (kReal - mirrorReal);
        const double diffImag = 0.5 * (kImag + mirrorImag);
        const double oddReal  = diffImag;
        const double oddImag  = -diffReal;

        const double twiddleReal = cos_[k];
        const double twiddleImag = sin_[k];

        const double rotatedReal = (twiddleReal * oddReal) - (twiddleImag * oddImag);
        const double rotatedImag = (twiddleReal * oddImag) + (twiddleImag * oddReal);

        real[k]      = evenReal + rotatedReal;
        imaginary[k] = evenImag + rotatedImag;

        // Skipped when the pair meets itself at N/4, where the two formulas
        // agree and the write would be redundant rather than wrong.
        if (mirror != k) {
            real[mirror]      = evenReal - rotatedReal;
            imaginary[mirror] = rotatedImag - evenImag;
        }
    }
}

void RealFft::inverse(double* real, double* imaginary, double* output) const {
    const std::size_t half = size_ / 2;

    // The forward pass run backwards. Each pair (k, half-k) is rebuilt into the
    // packed spectrum the half-length inverse expects.
    //
    // The factors of two that the forward pass divided out are simply not
    // reintroduced here: carrying 2E and 2O through gives 2Z, and the
    // half-length unnormalised inverse turns that into N*z rather than
    // (N/2)*z -- which is the N this transform's contract promises. Dividing
    // here and multiplying later would be the same arithmetic, spelled twice.
    const double zeroReal = real[0];
    const double nyquist  = real[half];
    real[0]      = zeroReal + nyquist;
    imaginary[0] = zeroReal - nyquist;

    for (std::size_t k = 1; k <= size_ / 4; ++k) {
        const std::size_t mirror = half - k;

        const double kReal      = real[k];
        const double kImag      = imaginary[k];
        const double mirrorReal = real[mirror];
        const double mirrorImag = imaginary[mirror];

        // 2E = X[k] + conj(X[half-k]); 2*W*O = X[k] - conj(X[half-k]).
        const double evenReal = kReal + mirrorReal;
        const double evenImag = kImag - mirrorImag;
        const double sumReal  = kReal - mirrorReal;
        const double sumImag  = kImag + mirrorImag;

        // Undo the forward rotation. W has unit magnitude, so its inverse is its
        // conjugate and no division is needed.
        const double twiddleReal = cos_[k];
        const double twiddleImag = -sin_[k];

        const double oddReal = (twiddleReal * sumReal) - (twiddleImag * sumImag);
        const double oddImag = (twiddleReal * sumImag) + (twiddleImag * sumReal);

        // Z = E + i*O, and Z[half-k] = conj(E) + i*conj(O).
        real[k]      = evenReal - oddImag;
        imaginary[k] = evenImag + oddReal;

        if (mirror != k) {
            real[mirror]      = evenReal + oddImag;
            imaginary[mirror] = oddReal - evenImag;
        }
    }

    half_.inverse(real, imaginary);

    for (std::size_t n = 0; n < half; ++n) {
        output[2 * n]     = real[n];
        output[2 * n + 1] = imaginary[n];
    }
}

}  // namespace xpcog
