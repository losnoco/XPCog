// FreeSurround: a matrix surround decoder, stereo to 5.1 and friends.
//
// Christian Kothe's algorithm, by way of Cog's Audio/ThirdParty/fsurround. The
// channel allocation tables are vendored verbatim (vendor/fsurround); this is
// the decoder, rewritten, and the rewrite is confined to one thing: Cog's copy
// computes its transform with vDSP, which exists only on Apple platforms. Every
// vDSP call is replaced by the equivalent over RealFft or a plain loop.
//
// The rewrite is *only* that. Everything else is reproduced deliberately,
// including several things that look like mistakes and are:
//
//   * `pi` is a float, 3.141592654f, used throughout double expressions. The
//     window function and the LFE crossover are both built from it.
//   * The helpers that square, clamp, compare and take signs all return float
//     from double arguments. `amplitude()` therefore rounds each squared
//     component to float *before* adding them. This is not a rounding that can
//     be tidied away -- it is in the signal path of every bin.
//
// Those are upstream's, they are what produced the golden capture in
// tests/golden, and correcting them would be a different decoder that sounds
// slightly different. If they are ever to be fixed, the fixture has to be
// recaptured in the same commit and the change described as what it is.
//
// One genuine difference from Cog, and it is a fidelity fix rather than a
// deviation: see the scaling note in the .cpp.
//
// Not a DSPNode, for the reason Downmix is not one -- it changes the channel
// count, so it belongs where channel geometry already changes. See Downmix.hpp.

#pragma once

#include "freesurround_channels.h"
#include "xpcog/core/audio/RealFft.hpp"

#include <cstddef>
#include <vector>

namespace xpcog {

class FreeSurround {
public:
    /// `blockSize` is the granularity decode() works at, a power of two, and
    /// should be around 10 ms of single-channel samples -- 4096 at 44.1 kHz,
    /// which is what Cog uses. Shorter or longer than 5 to 20 ms changes the
    /// granularity at which positions are decoded, so it is not free tuning.
    FreeSurround(channel_setup setup, unsigned blockSize);

    FreeSurround(const FreeSurround&)            = delete;
    FreeSurround& operator=(const FreeSurround&) = delete;

    /// Consumes exactly `blockSize()` interleaved stereo frames and returns a
    /// pointer to `blockSize()` interleaved frames of `channels()` channels.
    ///
    /// The output is delayed by half a block. The buffer is the decoder's own
    /// and stays valid until the next call.
    const float* decode(const float* input);

    /// Drops the overlap-add history. For a seek, where carrying half a block
    /// of the old position into the new one is exactly the smear a listener is
    /// waiting to hear land.
    void flush();

    [[nodiscard]] unsigned blockSize() const noexcept { return blockSize_; }
    [[nodiscard]] unsigned channels() const noexcept { return channels_; }

    /// Frames of output the decoder is holding back: half a block once started.
    [[nodiscard]] unsigned buffered() const noexcept;

    // Soundfield controls, in the order they are applied. Cog exposes none of
    // these beyond the fixed values it sets at construction.
    void setCircularWrap(float degrees) noexcept { circularWrap_ = degrees; }
    void setShift(float v) noexcept { shift_ = v; }
    void setDepth(float v) noexcept { depth_ = v; }
    void setFocus(float v) noexcept { focus_ = v; }
    void setFrontSeparation(float v) noexcept { frontSeparation_ = v; }
    void setRearSeparation(float v) noexcept { rearSeparation_ = v; }
    void setBassRedirection(bool v) noexcept { useLfe_ = v; }

    /// Cutoffs are given normalised to the Nyquist rate, as Cog gives them:
    /// `hz / (sampleRate / 2)`.
    void setLowCutoff(float normalised) noexcept;
    void setHighCutoff(float normalised) noexcept;

    /// Present for parity with Cog's API and **deliberately does nothing**.
    ///
    /// Upstream stores this and never reads it -- there is no `center_image` in
    /// the decode path at all. Cog sets it to 0.7 and gets the same output it
    /// would get at any other value. Keeping the setter and saying so is more
    /// use than dropping it, because the next person to look will otherwise
    /// wonder where the control went; implementing it would be inventing an
    /// algorithm, not porting one.
    void setCenterImage(float) noexcept {}

    [[nodiscard]] static unsigned   channelCount(channel_setup setup);
    [[nodiscard]] static channel_id channelAt(channel_setup setup, unsigned index);

private:
    void decodeHalfBlock(const float* input);

    unsigned      blockSize_;
    unsigned      channels_;
    channel_setup setup_;

    float circularWrap_    = 90.0F;
    float shift_           = 0.0F;
    float depth_           = 1.0F;
    float focus_           = 0.0F;
    float frontSeparation_ = 1.0F;
    float rearSeparation_  = 1.0F;
    float lowCut_          = 0.0F;
    float highCut_         = 0.0F;
    bool  useLfe_          = false;
    bool  bufferEmpty_     = true;

    RealFft fft_;

    std::vector<double> window_;
    std::vector<double> left_;
    std::vector<double> right_;
    std::vector<double> timeDomain_;
    std::vector<float>  timeDomainFloat_;

    /// Split complex, one pair per spectrum: the transform's own layout, and the
    /// one the steering loop indexes bin by bin.
    std::vector<double> leftReal_;
    std::vector<double> leftImag_;
    std::vector<double> rightReal_;
    std::vector<double> rightImag_;
    /// One spectrum per output channel, each `fft_.bins()` long.
    std::vector<std::vector<double>> signalReal_;
    std::vector<std::vector<double>> signalImag_;

    std::vector<float> inputBuffer_;
    std::vector<float> outputBuffer_;
};

}  // namespace xpcog
