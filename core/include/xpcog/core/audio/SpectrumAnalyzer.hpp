// The spectrum, as Cog draws it. Port of the parts of deadbeef's ddb_analyzer that
// Cog actually configures (Visualization/ThirdParty/deadbeef/analyzer.c) plus the
// FFT stage from Audio/ThirdParty/deadbeef/fft_pffft.c.
//
// The numbers are Cog's, taken from where it sets them rather than from the
// library's defaults, which differ:
//
//   window          4096 samples          fft_calculate(..., 2048): a complex
//                                         transform of 2 * fft_size
//   bins            2048                  the lower half of that transform
//   window shape    Hamming               vDSP_hamm_window
//   scaling         2 / 2048              `mult` in fft_calculate
//   bands           semitones from C0     OCTAVES 11, STEPS 24, octave_bars_step 2
//   range           10 Hz .. 22 kHz       SpectrumViewCG, not the library's 50 Hz
//   floor           -80 dB                LOWER_BOUND
//   peak hold       10 frames             peak_hold
//
// Two of those are worth pausing on.
//
// The band series is a *tempered scale*, not a set of equal log divisions: bands sit
// on musical notes, at C0 * 2^(i/24) taking every second quarter-tone, so each bar
// is a semitone. That is why a spectrum of music lines up with the notes being
// played, and it is a deliberate choice of deadbeef's worth keeping rather than
// replacing with something more obvious.
//
// The 2/2048 scaling looks arbitrary and is not: it puts a full-scale sine at
// roughly 0 dB. A Hamming window sums to about 0.54N, so a full-scale tone's bin
// reaches A * 0.54 * 4096/2, and 2/2048 of that is about 1.08 -- call it 0 dBFS.
// So the -80 dB floor really is 80 dB below full scale, and there is a test that
// pins it.
//
// Qt-free, like everything in core, and it holds no audio of its own -- analyze()
// is handed a window by whoever is drawing.

#pragma once

#include "xpcog/core/audio/Fft.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace xpcog {

class SpectrumAnalyzer {
public:
    /// Samples per analysis window. Cog's `fft_size * 2`.
    static constexpr std::size_t kWindowFrames = 4096;
    /// Usable bins: the lower half of the transform.
    static constexpr std::size_t kBins = kWindowFrames / 2;

    /// Cog's LOWER_BOUND. Anything quieter is drawn as nothing.
    static constexpr double kFloorDb = -80.0;

    SpectrumAnalyzer();

    /// Sizes the band table for `sampleRate`. Bands above Nyquist are dropped
    /// rather than folded, so a 44.1 kHz track shows fewer bars than a 96 kHz one --
    /// which is honest: there is no content up there to show.
    void prepare(double sampleRate);

    /// Analyses one window and advances the peak hold by one frame.
    ///
    /// `frames` shorter than kWindowFrames is zero-padded, so a display can start
    /// before the tap has filled.
    void analyze(const float* mono, std::size_t frames);

    /// Band levels, 0 at the floor and 1 at full scale, lowest frequency first.
    [[nodiscard]] const std::vector<float>& bands() const noexcept { return bands_; }

    /// The held peak per band, on the same scale.
    [[nodiscard]] const std::vector<float>& peaks() const noexcept { return peaks_; }

    /// Each band's centre frequency, for axis labels.
    [[nodiscard]] const std::vector<double>& frequencies() const noexcept {
        return frequencies_;
    }

    /// Drops the levels and the peaks. For a stop or a track change.
    void reset();

    /// The raw magnitudes of the last analyze(), before band grouping. Exposed for
    /// tests -- a band level cannot distinguish a wrong window from a wrong band
    /// mapping, and these can.
    [[nodiscard]] const std::vector<float>& magnitudes() const noexcept {
        return magnitudes_;
    }

private:
    void buildBands(double sampleRate);

    std::unique_ptr<Fft> fft_;
    double               sampleRate_ = 0.0;

    std::vector<float> window_;      ///< Hamming, kWindowFrames long
    std::vector<float> real_;        ///< transform scratch
    std::vector<float> imaginary_;
    std::vector<float> magnitudes_;  ///< kBins, scaled as Cog scales them

    /// The first bin of each band, plus a final sentinel so band i covers
    /// [edges_[i], edges_[i + 1]).
    std::vector<std::size_t> edges_;
    std::vector<double>      frequencies_;

    std::vector<float> bands_;
    std::vector<float> peaks_;
    std::vector<int>   holds_;
};

}  // namespace xpcog
