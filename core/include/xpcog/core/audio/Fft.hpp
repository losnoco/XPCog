// A radix-2 complex FFT, for the spectrum analyser.
//
// Cog uses pffft here (Audio/ThirdParty/deadbeef/fft_pffft.c, itself from
// deadbeef) with vDSP for the windowing and magnitudes. This is written out
// instead, and the reason is proportion rather than preference: the analyser needs
// one 4096-point transform per displayed frame -- about 250k butterflies a second
// at 60 fps, which is nothing -- so a new third-party dependency would be bought
// entirely for a speed nobody can measure here. pffft earns its place in a plugin
// host running dozens of these; it does not earn it for one spectrum strip.
//
// The trade is that correctness is now ours to establish, so it is established the
// only way that is worth anything: the tests check this against a naive O(n^2) DFT
// on random input, which shares no code and no structure with it. An FFT that
// agrees with a direct evaluation of the transform's definition, at every bin, is
// right -- and the usual failure modes (a missed bit-reversal, a twiddle of the
// wrong sign, a stage stride off by one) all break that agreement loudly rather
// than subtly.
//
// Split complex -- separate real and imaginary arrays -- rather than interleaved
// pairs. Cog's route writes real input into every other slot of an interleaved
// buffer with a vDSP stride, which is where an off-by-one becomes a silent
// frequency shift; two arrays cannot express that mistake.
//
// Not real-time safe to *construct* (it allocates its tables), and deliberately
// so: build one, keep it, and call forward() as often as you like. forward()
// allocates nothing.

#pragma once

#include <cstddef>
#include <vector>

namespace xpcog {

class Fft {
public:
    /// `size` must be a power of two and at least 2. Precomputes the bit-reversal
    /// permutation and the twiddle factors.
    explicit Fft(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// Forward transform, in place, over `size()` elements of each array.
    ///
    /// Unnormalised, matching the convention every FFT library uses and the one
    /// Cog's scaling assumes: the analyser applies its own 2/N.
    void forward(float* real, float* imaginary) const;

private:
    std::size_t              size_;
    std::vector<std::size_t> reversed_;
    /// cos and sin of -2*pi*k/size for k < size/2. Held as double because they are
    /// computed once and reused for every transform -- the rounding saved is free.
    std::vector<double> cos_;
    std::vector<double> sin_;
};

}  // namespace xpcog
