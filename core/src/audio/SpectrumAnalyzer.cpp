#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace xpcog {
namespace {

// deadbeef's constants, spelled as it spells them (analyzer.c:30-33).
constexpr int    kOctaves = 11;
constexpr int    kSteps   = 24;  ///< quarter-tones per octave
constexpr double kRoot24  = 1.0293022366;  ///< 2^(1/24)
constexpr double kC0      = 16.3515978313;  ///< 440 * kRoot24^-114

/// Cog's SpectrumViewCG, not the library's defaults.
constexpr double kMinFrequency = 10.0;
constexpr double kMaxFrequency = 22000.0;
/// Every second quarter-tone, so one bar per semitone (octave_bars_step = 2).
constexpr int kBandStep = 2;

/// Frames a peak stays put before it starts falling. Cog's peak_hold.
constexpr int kPeakHoldFrames = 10;
/// How fast it falls once it lets go, in normalised units per frame. Chosen rather
/// than ported: Cog's peak_speed_scale is expressed against a view height in points
/// and a frame duration, neither of which exists in core. Roughly a second from full
/// scale to the floor at 60 fps, which reads as a peak marker rather than a
/// second bar.
constexpr float kPeakDecayPerFrame = 0.016F;

/// Cog's `mult` from fft_calculate: 2 / fft_size, where fft_size is half the window.
constexpr float kMagnitudeScale = 2.0F / static_cast<float>(SpectrumAnalyzer::kBins);

}  // namespace

SpectrumAnalyzer::SpectrumAnalyzer()
    : fft_(std::make_unique<Fft>(kWindowFrames)),
      window_(kWindowFrames),
      real_(kWindowFrames),
      imaginary_(kWindowFrames),
      magnitudes_(kBins, 0.0F) {
    // Hamming, matching vDSP_hamm_window with no flags: the periodic form, over the
    // whole window rather than the window minus one.
    for (std::size_t index = 0; index < kWindowFrames; ++index) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(index) /
                             static_cast<double>(kWindowFrames);
        window_[index] = static_cast<float>(0.54 - (0.46 * std::cos(phase)));
    }
}

void SpectrumAnalyzer::prepare(double sampleRate) {
    if (sampleRate <= 0.0 || sampleRate == sampleRate_) {
        return;
    }
    sampleRate_ = sampleRate;
    buildBands();
    reset();
}

void SpectrumAnalyzer::setMode(Mode mode) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    buildBands();
    reset();
}

void SpectrumAnalyzer::setFrequencyBandCount(std::size_t bars) {
    // At least a handful, and never more bars than there are bins to fill them --
    // past that the extra bars are duplicates of their neighbours, which is the
    // thing NoteBands mode has a good reason for and this mode does not.
    const std::size_t clamped = std::clamp(bars, std::size_t{8}, kBins);
    if (clamped == frequencyBandCount_) {
        return;
    }
    frequencyBandCount_ = clamped;
    if (mode_ == Mode::Frequencies) {
        buildBands();
        reset();
    }
}

void SpectrumAnalyzer::setFloorDb(double decibels) {
    if (decibels >= 0.0 || decibels == floorDb_) {
        return;
    }
    // No rebuild: the floor only scales the dB-to-level mapping, and the bands
    // themselves are unchanged. Takes effect on the next analyze().
    floorDb_ = decibels;
}

void SpectrumAnalyzer::buildBands() {
    bins_.clear();
    ratios_.clear();
    frequencies_.clear();

    if (sampleRate_ <= 0.0) {
        return;
    }

    const double binWidth = sampleRate_ / static_cast<double>(kWindowFrames);
    // Nothing above Nyquist, and nothing above Cog's ceiling either.
    const double ceiling = std::min(kMaxFrequency, sampleRate_ / 2.0);

    if (mode_ == Mode::Frequencies) {
        buildFrequencyBands(binWidth, ceiling);
    } else {
        buildNoteBands(binWidth, ceiling);
    }

    resizeBands();
}

void SpectrumAnalyzer::addBand(double frequency, double binWidth) {
    const double position = frequency / binWidth;
    const auto   bin      = static_cast<std::size_t>(position);
    if (bin >= kBins) {
        return;
    }
    bins_.push_back(bin);
    // Where in the bin the frequency sits, as a fraction of the way to the next
    // one. deadbeef takes this fraction in log frequency (_generate_octave_note_bars,
    // analyzer.c), which between two adjacent bins differs from the linear one by
    // less than the interpolation is worth -- and divides by log10(0) when a band
    // lands in bin 0, which C0 does at 96 kHz. Linear, then.
    ratios_.push_back(static_cast<float>(position - static_cast<double>(bin)));
    frequencies_.push_back(frequency);
}

void SpectrumAnalyzer::resizeBands() {
    bands_.assign(frequencies_.size(), 0.0F);
    peaks_.assign(frequencies_.size(), 0.0F);
    holds_.assign(frequencies_.size(), 0);
}

void SpectrumAnalyzer::buildFrequencyBands(double binWidth, double ceiling) {
    // Evenly spaced in log frequency across the same range the note bands use, so
    // switching modes does not also change what part of the spectrum is on screen.
    //
    // Repeated bins are kept, exactly as in NoteBands mode. Dropping them was the
    // first attempt here too, on the argument that these positions carry no meaning
    // beyond "evenly spread" -- which gets it backwards: even spread *is* the
    // meaning of this mode, and at the bottom of the range, where a dozen requested
    // frequencies fall inside one 10.8 Hz bin, dropping the duplicates leaves an
    // axis whose steps jump from a ratio of 1.13 to 2.08. A test caught that. The
    // rule turns out to be the same in both modes: the band table describes where
    // the bars are, and the resolution of the window is not its business.
    const double lowest  = std::log10(kMinFrequency);
    const double highest = std::log10(ceiling);
    const auto   count   = static_cast<double>(frequencyBandCount_);

    for (std::size_t index = 0; index < frequencyBandCount_; ++index) {
        const double position  = static_cast<double>(index) / (count - 1.0);
        const double frequency = std::pow(10.0, lowest + (position * (highest - lowest)));
        addBand(frequency, binWidth);
    }
}

void SpectrumAnalyzer::buildNoteBands(double binWidth, double ceiling) {
    for (int step = 0; step < kOctaves * kSteps; step += kBandStep) {
        const double frequency = kC0 * std::pow(kRoot24, step);
        if (frequency < kMinFrequency || frequency > ceiling) {
            continue;
        }

        // Adjacent semitones below a few hundred hertz land in the *same* bin: at
        // 44.1 kHz a 4096-point window resolves 10.8 Hz, and a semitone down at C1
        // is under two. Those bands are kept anyway, sharing the bin, which is what
        // Cog does.
        //
        // Dropping the duplicates was the first attempt and it was wrong, for a
        // reason worth recording: the whole point of a tempered scale rather than
        // equal log divisions is that the bars line up with notes, so an axis with
        // gaps in it defeats the choice of series. What tells the bars apart is
        // the interpolation in analyze(): each reads the spectrum at its own
        // frequency, between the bin and the next, so a run of bars in one bin
        // slopes towards the neighbouring bin's level rather than standing at one
        // height -- the same picture deadbeef draws, from the same ratio.
        addBand(frequency, binWidth);
    }
}

void SpectrumAnalyzer::analyze(const float* mono, std::size_t frames) {
    if (frequencies_.empty() || mono == nullptr) {
        return;
    }

    const std::size_t usable = std::min(frames, kWindowFrames);
    for (std::size_t index = 0; index < usable; ++index) {
        real_[index]      = mono[index] * window_[index];
        imaginary_[index] = 0.0F;
    }
    // A short window is padded rather than refused, so the display starts at once.
    for (std::size_t index = usable; index < kWindowFrames; ++index) {
        real_[index]      = 0.0F;
        imaginary_[index] = 0.0F;
    }

    fft_->forward(real_.data(), imaginary_.data());

    for (std::size_t bin = 0; bin < kBins; ++bin) {
        magnitudes_[bin] = std::hypot(real_[bin], imaginary_[bin]) * kMagnitudeScale;
    }

    const std::size_t count = frequencies_.size();
    for (std::size_t band = 0; band < count; ++band) {
        // The level at the band's own frequency, read between its bin and the
        // next. This is deadbeef's _interpolate_bin_with_ratio, and it is what
        // separates the bars at the bottom of the range, where several semitones
        // share one bin: each takes the spectrum at its own point in that bin,
        // rather than all of them the bin's value. In dB, as deadbeef's data is by
        // the time it interpolates -- a straight line between two levels on the
        // scale the bars are drawn on.
        const std::size_t first = bins_[band];
        double            decibels = binDecibels(first);
        if (first + 1 < kBins) {
            decibels += (binDecibels(first + 1) - decibels) *
                        static_cast<double>(ratios_[band]);
        }

        // A band wider than a bin then takes the loudest of the bins it covers
        // beyond the first, up to the next band's -- not the mean. A band at the
        // top of the range spans dozens of bins, and averaging them buries a tone
        // in the noise floor either side of it: a spectrum where a 10 kHz sine
        // barely registers while its neighbours do. The maximum is what makes a
        // peak look like a peak, and it is what deadbeef's accumulation does. The
        // last band runs to the top of the bins.
        const std::size_t stop = (band + 1 < count) ? bins_[band + 1] : kBins;
        for (std::size_t bin = first + 1; bin < stop; ++bin) {
            decibels = std::max(decibels, binDecibels(bin));
        }

        // Normalised so 0 is the floor and 1 is full scale. The magnitude scaling
        // above is what makes "full scale" mean a full-scale sine.
        const auto level = static_cast<float>(
            std::clamp((decibels - floorDb_) / -floorDb_, 0.0, 1.0));

        bands_[band] = level;

        if (level >= peaks_[band]) {
            peaks_[band] = level;
            holds_[band] = kPeakHoldFrames;
        } else if (holds_[band] > 0) {
            --holds_[band];
        } else {
            peaks_[band] = std::max(level, peaks_[band] - kPeakDecayPerFrame);
        }
    }
}

double SpectrumAnalyzer::binDecibels(std::size_t bin) const {
    // Clamped at the floor before any interpolation, so a bin of near-silence
    // (-300 dB, or nothing at all) pulls its neighbour's level down to the floor
    // and no further. Below the floor there is nothing to draw anyway.
    const float magnitude = magnitudes_[bin];
    if (magnitude <= 0.0F) {
        return floorDb_;
    }
    return std::max(20.0 * std::log10(static_cast<double>(magnitude)), floorDb_);
}

void SpectrumAnalyzer::reset() {
    std::fill(bands_.begin(), bands_.end(), 0.0F);
    std::fill(peaks_.begin(), peaks_.end(), 0.0F);
    std::fill(holds_.begin(), holds_.end(), 0);
    std::fill(magnitudes_.begin(), magnitudes_.end(), 0.0F);
}

}  // namespace xpcog
