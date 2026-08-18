// A real-input FFT in double precision, built on the complex one.
//
// FreeSurround transforms real audio, and doing that through a complex
// transform of the same length wastes half of it: the input's imaginary part is
// zero and the output is conjugate-symmetric, so half the arithmetic computes
// numbers the other half already implies. The standard answer, and the one
// vDSP's zrop uses, is to pack N reals into N/2 complex values -- evens into the
// real part, odds into the imaginary -- transform those, and untangle the result
// with one pass of twiddles. That is what this is.
//
// It is a wrapper rather than a new transform. The butterflies, the table and
// the bit-reversal are Fft's, at half the length; what is added here is the
// packing and the untangling. That matters for how it is tested: Fft is already
// checked against a direct DFT, so the only thing this file can get wrong is the
// untangling, and the tests check it the same way -- against the definition,
// bin by bin.
//
// The conventions, both of which are load-bearing:
//
//   * `forward` produces bins 0..N/2 inclusive -- N/2+1 of them, DC and Nyquist
//     included as ordinary entries with zero imaginary part. Not vDSP's packed
//     layout, which hides Nyquist in the imaginary slot of DC. The packed form
//     saves one complex value per transform and costs a reader the one piece of
//     knowledge they are least likely to have; FreeSurround discards both of
//     those bins anyway.
//   * Both directions are **unnormalised**, so `inverse(forward(x)) == N * x`.
//     FreeSurround's window is sqrt(hann(k)/N) applied on analysis and again on
//     synthesis, which is where its 1/N lives. A transform that also divided
//     would apply it twice.
//
// Deliberately *not* reproducing vDSP's scaling. Apple's real transforms carry a
// factor-of-two convention that Cog's kernel silently depends on; folding that
// in here would put a mystery constant inside a transform that otherwise matches
// its own definition. It belongs in the kernel, named, where the golden capture
// can pin it -- see tools/fsurround-golden/.

#pragma once

#include "xpcog/core/audio/Fft.hpp"

#include <cstddef>
#include <vector>

namespace xpcog {

class RealFft {
public:
    /// `size` is N, the number of real samples. A power of two, at least 4 --
    /// the half-length complex transform needs at least 2.
    explicit RealFft(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// The number of bins each direction reads or writes: `size()/2 + 1`.
    [[nodiscard]] std::size_t bins() const noexcept { return size_ / 2 + 1; }

    /// `input` holds `size()` reals; `real` and `imaginary` hold `bins()` each
    /// and are overwritten. Allocates nothing.
    void forward(const double* input, double* real, double* imaginary) const;

    /// `real` and `imaginary` hold `bins()` each and are **destroyed** --
    /// the untangling runs in place, which is what lets this allocate nothing
    /// and hold no scratch. `output` receives `size()` reals.
    ///
    /// Consuming the input is not a compromise: it is what the caller wants.
    /// FreeSurround back-transforms each channel's spectrum once and never looks
    /// at it again, which is also how Cog's vDSP path works.
    void inverse(double* real, double* imaginary, double* output) const;

private:
    std::size_t         size_;
    Fft                 half_;
    /// cos and sin of -2*pi*k/N for k <= N/4, the only ones the untangling asks
    /// for: it walks pairs (k, N/2-k) inward and needs one twiddle per pair.
    std::vector<double> cos_;
    std::vector<double> sin_;
};

}  // namespace xpcog
