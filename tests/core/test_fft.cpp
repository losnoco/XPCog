// The FFT, against the definition of the transform it implements.
//
// This is the whole point of having written one rather than linking pffft: the
// correctness has to come from somewhere, and it comes from here. The reference is
// a naive O(n^2) DFT that evaluates the sum directly -- it shares no code, no
// tables and no structure with the thing under test, so agreement between them at
// every bin is real evidence rather than a restatement.
//
// The failure modes worth naming, because each one produces output that still looks
// like a spectrum:
//
//   * A bit-reversal that swaps every pair twice is the identity permutation. The
//     transform then computes something self-consistent, of the wrong signal.
//   * A twiddle of the wrong sign gives the inverse transform, which for real input
//     mirrors the spectrum -- and a mirrored spectrum of music still looks like
//     music.
//   * A stage stride off by one factor of two puts energy in the wrong bins,
//     smoothly.
//
// None of those survive a bin-by-bin comparison against the sum, which is why the
// first test uses random input: a sine would agree in the bins that matter and hide
// the rest in near-zero noise.

#include "xpcog/core/audio/Fft.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

using Catch::Approx;
using xpcog::Fft;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The transform, written as its definition. Deliberately the slow way.
std::vector<std::complex<double>> naiveDft(const std::vector<float>& real,
                                           const std::vector<float>& imaginary) {
    const std::size_t                 size = real.size();
    std::vector<std::complex<double>> out(size);
    for (std::size_t bin = 0; bin < size; ++bin) {
        std::complex<double> sum{0.0, 0.0};
        for (std::size_t n = 0; n < size; ++n) {
            const double angle = -kTwoPi * static_cast<double>(bin) *
                                 static_cast<double>(n) / static_cast<double>(size);
            const std::complex<double> sample{static_cast<double>(real[n]),
                                              static_cast<double>(imaginary[n])};
            sum += sample * std::complex<double>{std::cos(angle), std::sin(angle)};
        }
        out[bin] = sum;
    }
    return out;
}

}  // namespace

TEST_CASE("the FFT agrees with a direct DFT on random input", "[audio][fft]") {
    // 256 rather than the analyser's 4096: the reference is quadratic, and the
    // properties being checked do not depend on the size. Several sizes, because a
    // stride mistake can be invisible at one of them.
    std::mt19937                          rng(20260817);
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);

    for (const std::size_t size : {std::size_t{2}, std::size_t{8}, std::size_t{64},
                                   std::size_t{256}}) {
        std::vector<float> real(size);
        std::vector<float> imaginary(size);
        for (std::size_t n = 0; n < size; ++n) {
            real[n]      = noise(rng);
            imaginary[n] = noise(rng);
        }

        const std::vector<std::complex<double>> expected = naiveDft(real, imaginary);

        Fft fft(size);
        fft.forward(real.data(), imaginary.data());

        for (std::size_t bin = 0; bin < size; ++bin) {
            INFO("size " << size << " bin " << bin);
            // Scaled to the transform's magnitude: an absolute margin alone would be
            // either too loose for the small bins or too tight for the large ones.
            const double scale = std::max(1.0, std::abs(expected[bin]));
            REQUIRE(real[bin] == Approx(expected[bin].real()).margin(1e-3 * scale));
            REQUIRE(imaginary[bin] ==
                    Approx(expected[bin].imag()).margin(1e-3 * scale));
        }
    }
}

TEST_CASE("a real sine lands in its own bin and its mirror", "[audio][fft]") {
    // The property the analyser actually relies on, stated separately because the
    // comparison above would pass even if the bin *ordering* were reversed -- it
    // compares against a DFT computed with the same index convention.
    //
    // A sine at exactly bin k of a real signal puts its energy at k and at N-k, and
    // nowhere else. If the ordering were inverted this still holds by symmetry, so
    // the asymmetric part is what matters: an offset sine, whose DC lands at bin 0
    // and cannot be mirrored anywhere.
    constexpr std::size_t kSize = 64;
    constexpr std::size_t kBin  = 5;

    std::vector<float> real(kSize);
    std::vector<float> imaginary(kSize, 0.0F);
    for (std::size_t n = 0; n < kSize; ++n) {
        const double phase = kTwoPi * static_cast<double>(kBin) *
                             static_cast<double>(n) / static_cast<double>(kSize);
        real[n] = static_cast<float>(0.5 + std::sin(phase));
    }

    Fft fft(kSize);
    fft.forward(real.data(), imaginary.data());

    const auto magnitude = [&](std::size_t bin) {
        return std::hypot(static_cast<double>(real[bin]),
                          static_cast<double>(imaginary[bin]));
    };

    // DC: the constant offset times the length.
    REQUIRE(magnitude(0) == Approx(0.5 * kSize).margin(1e-3));
    // The tone, half the amplitude in each of the two conjugate bins.
    REQUIRE(magnitude(kBin) == Approx(0.5 * kSize).margin(1e-3));
    REQUIRE(magnitude(kSize - kBin) == Approx(0.5 * kSize).margin(1e-3));

    for (std::size_t bin = 1; bin < kSize; ++bin) {
        if (bin == kBin || bin == kSize - kBin) {
            continue;
        }
        INFO("bin " << bin << " should be empty");
        REQUIRE(magnitude(bin) < 1e-3);
    }
}

TEST_CASE("the transform is linear", "[audio][fft]") {
    // Cheap, and it catches a class the DFT comparison cannot: state left behind
    // between calls. The tables are const, but a future in-place scratch buffer
    // would break this and nothing else.
    constexpr std::size_t kSize = 32;
    Fft                   fft(kSize);

    std::vector<float> aReal(kSize);
    std::vector<float> aImag(kSize, 0.0F);
    std::vector<float> bReal(kSize);
    std::vector<float> bImag(kSize, 0.0F);
    for (std::size_t n = 0; n < kSize; ++n) {
        aReal[n] = static_cast<float>(std::sin(0.3 * static_cast<double>(n)));
        bReal[n] = static_cast<float>(std::cos(1.1 * static_cast<double>(n)));
    }

    std::vector<float> sumReal(kSize);
    std::vector<float> sumImag(kSize, 0.0F);
    for (std::size_t n = 0; n < kSize; ++n) {
        sumReal[n] = aReal[n] + bReal[n];
    }

    fft.forward(aReal.data(), aImag.data());
    fft.forward(bReal.data(), bImag.data());
    fft.forward(sumReal.data(), sumImag.data());

    for (std::size_t bin = 0; bin < kSize; ++bin) {
        INFO("bin " << bin);
        REQUIRE(sumReal[bin] == Approx(aReal[bin] + bReal[bin]).margin(1e-3));
        REQUIRE(sumImag[bin] == Approx(aImag[bin] + bImag[bin]).margin(1e-3));
    }
}
