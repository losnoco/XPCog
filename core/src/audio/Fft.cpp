#include "xpcog/core/audio/Fft.hpp"

#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace xpcog {
namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The number of bits needed to index `size` elements.
std::size_t bitsFor(std::size_t size) {
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < size) {
        ++bits;
    }
    return bits;
}

std::size_t reverseBits(std::size_t value, std::size_t bits) {
    std::size_t result = 0;
    for (std::size_t bit = 0; bit < bits; ++bit) {
        result = (result << 1U) | (value & 1U);
        value >>= 1U;
    }
    return result;
}

}  // namespace

Fft::Fft(std::size_t size) : size_(size) {
    // A power of two is not a convenience here, it is what makes the decimation
    // halve cleanly at every stage. Asserted rather than rounded: silently
    // transforming a different number of samples than the caller asked for would
    // shift every bin's centre frequency.
    assert(size_ >= 2 && (size_ & (size_ - 1)) == 0);

    const std::size_t bits = bitsFor(size_);
    reversed_.resize(size_);
    for (std::size_t index = 0; index < size_; ++index) {
        reversed_[index] = reverseBits(index, bits);
    }

    cos_.resize(size_ / 2);
    sin_.resize(size_ / 2);
    for (std::size_t k = 0; k < size_ / 2; ++k) {
        const double angle = -kTwoPi * static_cast<double>(k) /
                             static_cast<double>(size_);
        cos_[k] = std::cos(angle);
        sin_[k] = std::sin(angle);
    }
}

void Fft::forward(float* real, float* imaginary) const {
    transform(real, imaginary, false);
}

void Fft::forward(double* real, double* imaginary) const {
    transform(real, imaginary, false);
}

void Fft::inverse(double* real, double* imaginary) const {
    transform(real, imaginary, true);
}

template <typename Sample>
void Fft::transform(Sample* real, Sample* imaginary, bool invert) const {
    // Decimation in time: reorder into bit-reversed indices, after which the
    // butterflies run in place over contiguous pairs.
    //
    // Guarded by `index < target` so each pair is swapped once. Without it every
    // swap is undone by its mirror and the permutation is the identity -- which
    // leaves an FFT that still produces plausible-looking output, just of the wrong
    // signal. One of the things the DFT comparison in the tests exists to catch.
    for (std::size_t index = 0; index < size_; ++index) {
        const std::size_t target = reversed_[index];
        if (index < target) {
            std::swap(real[index], real[target]);
            std::swap(imaginary[index], imaginary[target]);
        }
    }

    for (std::size_t length = 2; length <= size_; length <<= 1U) {
        const std::size_t half = length / 2;
        // Which twiddle each k within a butterfly group needs: the table holds
        // size_/2 entries for the whole transform, and a stage of this length walks
        // it in steps of size_/length.
        const std::size_t stride = size_ / length;

        for (std::size_t base = 0; base < size_; base += length) {
            for (std::size_t k = 0; k < half; ++k) {
                // The table holds sin(-2*pi*k/N). The inverse transform is the
                // same sum with the opposite exponent sign, so it needs the
                // negation and nothing else -- no second table, and no reversal
                // of the permutation or the stage order.
                const double twiddleReal = cos_[k * stride];
                const double twiddleImag =
                    invert ? -sin_[k * stride] : sin_[k * stride];

                const std::size_t lower = base + k;
                const std::size_t upper = lower + half;

                // Widened explicitly, all four of them, rather than letting each
                // expression promote where it meets a double twiddle. The arithmetic
                // is identical; what changes is that the widening is stated once and
                // visibly, instead of happening eight times implicitly -- which is
                // what -Wdouble-promotion objects to, and it is right to.
                const double lowerReal = static_cast<double>(real[lower]);
                const double lowerImag = static_cast<double>(imaginary[lower]);
                const double upperReal = static_cast<double>(real[upper]);
                const double upperImag = static_cast<double>(imaginary[upper]);


                // The rotated upper half, computed before either slot is written --
                // the lower one is still needed below.
                const double rotatedReal =
                    (upperReal * twiddleReal) - (upperImag * twiddleImag);
                const double rotatedImag =
                    (upperReal * twiddleImag) + (upperImag * twiddleReal);

                real[upper]      = static_cast<Sample>(lowerReal - rotatedReal);
                imaginary[upper] = static_cast<Sample>(lowerImag - rotatedImag);
                real[lower]      = static_cast<Sample>(lowerReal + rotatedReal);
                imaginary[lower] = static_cast<Sample>(lowerImag + rotatedImag);
            }
        }
    }
}

// Only these two. A third would be a caller that has not decided what precision
// it works in.
template void Fft::transform<float>(float*, float*, bool) const;
template void Fft::transform<double>(double*, double*, bool) const;

}  // namespace xpcog
