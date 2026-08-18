// The real-input FFT, and the complex one's inverse, against their definitions.
//
// Same reasoning as test_fft.cpp: the value of having written the transform is
// that its correctness is established here rather than assumed, so the reference
// is a direct evaluation of the sum, sharing no code and no structure with the
// thing under test.
//
// What is specific to this file is *which* mistakes it is hunting. RealFft's
// butterflies are Fft's, already covered; the only new arithmetic is the
// packing and the untangling, and it fails in ways that still look like a
// spectrum:
//
//   * The mirror bin computed as conj(X[k]) instead of conj(E - W*O). That is
//     the natural guess, it is correct for a *whole* transform's Hermitian
//     symmetry, and it is wrong here -- which makes it exactly the mistake to
//     test for rather than to reason about.
//   * DC and Nyquist swapped, or Nyquist left in the imaginary slot of bin 0 in
//     the manner of vDSP's packed layout.
//   * A twiddle applied with the wrong sign, which mirrors the spectrum about
//     N/4 and leaves random input looking just as random.
//
// The round-trip test is deliberately not the only one. A forward and an inverse
// that are wrong in mirror-image ways round-trip perfectly, and that is not a
// hypothetical: the sign of the untangling twiddle is shared between them.

#include "xpcog/core/audio/Fft.hpp"
#include "xpcog/core/audio/RealFft.hpp"

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
using xpcog::RealFft;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// The transform, written as its definition, over a real signal.
std::vector<std::complex<double>> naiveRealDft(const std::vector<double>& input) {
    const std::size_t                 size = input.size();
    std::vector<std::complex<double>> out(size / 2 + 1);
    for (std::size_t bin = 0; bin < out.size(); ++bin) {
        std::complex<double> sum{0.0, 0.0};
        for (std::size_t n = 0; n < size; ++n) {
            const double angle = -kTwoPi * static_cast<double>(bin) *
                                 static_cast<double>(n) / static_cast<double>(size);
            sum += input[n] * std::complex<double>{std::cos(angle), std::sin(angle)};
        }
        out[bin] = sum;
    }
    return out;
}

std::vector<double> randomSignal(std::size_t size, unsigned seed) {
    std::mt19937                           rng(seed);
    std::uniform_real_distribution<double> spread(-1.0, 1.0);
    std::vector<double>                    out(size);
    for (double& sample : out) {
        sample = spread(rng);
    }
    return out;
}

}  // namespace

TEST_CASE("the real FFT agrees with a direct DFT on random input", "[audio][fft]") {
    // Several sizes, because the untangling walks pairs inward and meets itself
    // at N/4 -- a size where that midpoint lands differently is a different code
    // path through the same loop.
    for (const std::size_t size : {4U, 8U, 64U, 256U, 1024U}) {
        const std::vector<double> input = randomSignal(size, 1234U + size);

        const RealFft       fft(size);
        std::vector<double> real(fft.bins());
        std::vector<double> imaginary(fft.bins());
        fft.forward(input.data(), real.data(), imaginary.data());

        const std::vector<std::complex<double>> expected = naiveRealDft(input);
        REQUIRE(expected.size() == fft.bins());

        for (std::size_t bin = 0; bin < fft.bins(); ++bin) {
            INFO("size " << size << " bin " << bin);
            REQUIRE(real[bin] == Approx(expected[bin].real()).margin(1e-9));
            REQUIRE(imaginary[bin] == Approx(expected[bin].imag()).margin(1e-9));
        }
    }
}

TEST_CASE("DC and Nyquist are real, and in their own bins", "[audio][fft]") {
    // The two bins vDSP packs together, checked apart. DC is the sum of the
    // signal and Nyquist is its alternating sum, both by definition, and both
    // have no imaginary part.
    constexpr std::size_t     kSize = 64;
    const std::vector<double> input = randomSignal(kSize, 99U);

    const RealFft       fft(kSize);
    std::vector<double> real(fft.bins());
    std::vector<double> imaginary(fft.bins());
    fft.forward(input.data(), real.data(), imaginary.data());

    double sum         = 0.0;
    double alternating = 0.0;
    for (std::size_t n = 0; n < kSize; ++n) {
        sum += input[n];
        alternating += (n % 2 == 0) ? input[n] : -input[n];
    }

    CHECK(real[0] == Approx(sum).margin(1e-9));
    CHECK(imaginary[0] == Approx(0.0).margin(1e-12));
    CHECK(real[kSize / 2] == Approx(alternating).margin(1e-9));
    CHECK(imaginary[kSize / 2] == Approx(0.0).margin(1e-12));
}

TEST_CASE("the real transform round-trips to N times the input", "[audio][fft]") {
    // The scaling is the contract, not an accident to be normalised away by
    // whoever notices it: FreeSurround's window carries the 1/N.
    constexpr std::size_t     kSize = 256;
    const std::vector<double> input = randomSignal(kSize, 7U);

    const RealFft       fft(kSize);
    std::vector<double> real(fft.bins());
    std::vector<double> imaginary(fft.bins());
    fft.forward(input.data(), real.data(), imaginary.data());

    std::vector<double> output(kSize);
    fft.inverse(real.data(), imaginary.data(), output.data());

    for (std::size_t n = 0; n < kSize; ++n) {
        INFO("sample " << n);
        REQUIRE(output[n] == Approx(static_cast<double>(kSize) * input[n]).margin(1e-9));
    }
}

TEST_CASE("the complex inverse undoes the complex forward", "[audio][fft]") {
    constexpr std::size_t kSize = 128;

    std::vector<double> real      = randomSignal(kSize, 21U);
    std::vector<double> imaginary = randomSignal(kSize, 22U);
    const std::vector<double> originalReal = real;
    const std::vector<double> originalImag = imaginary;

    const Fft fft(kSize);
    fft.forward(real.data(), imaginary.data());
    fft.inverse(real.data(), imaginary.data());

    for (std::size_t n = 0; n < kSize; ++n) {
        INFO("sample " << n);
        REQUIRE(real[n] == Approx(static_cast<double>(kSize) * originalReal[n]).margin(1e-9));
        REQUIRE(imaginary[n] ==
                Approx(static_cast<double>(kSize) * originalImag[n]).margin(1e-9));
    }
}

TEST_CASE("the double forward matches the float one", "[audio][fft]") {
    // Not a precision claim -- the butterflies were always double -- but the
    // template now has two instantiations and nothing else would notice if one
    // of them stopped being compiled from the same source.
    constexpr std::size_t kSize = 64;

    const std::vector<double> seedReal = randomSignal(kSize, 5U);
    const std::vector<double> seedImag = randomSignal(kSize, 6U);

    std::vector<float>  floatReal(kSize);
    std::vector<float>  floatImag(kSize);
    std::vector<double> doubleReal(kSize);
    std::vector<double> doubleImag(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        floatReal[n]  = static_cast<float>(seedReal[n]);
        floatImag[n]  = static_cast<float>(seedImag[n]);
        doubleReal[n] = static_cast<double>(floatReal[n]);
        doubleImag[n] = static_cast<double>(floatImag[n]);
    }

    const Fft fft(kSize);
    fft.forward(floatReal.data(), floatImag.data());
    fft.forward(doubleReal.data(), doubleImag.data());

    for (std::size_t n = 0; n < kSize; ++n) {
        INFO("bin " << n);
        REQUIRE(static_cast<double>(floatReal[n]) == Approx(doubleReal[n]).margin(1e-4));
        REQUIRE(static_cast<double>(floatImag[n]) == Approx(doubleImag[n]).margin(1e-4));
    }
}
