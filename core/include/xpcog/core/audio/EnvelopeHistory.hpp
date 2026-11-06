// The last few seconds of what was heard, as an envelope that scrolls. The
// Waveform pane's model; Cog's WaveformView keeps the same four seconds
// (Visualization/WaveformView.swift, 5e472413d).
//
// A fixed number of columns, each the lowest and highest sample of its stretch
// of time, so the cost of drawing does not depend on the sample rate and the
// memory does not depend on anything.

#pragma once

#include <cstddef>
#include <vector>

namespace xpcog {

class EnvelopeHistory {
public:
    static constexpr double kSeconds = 4.0;

    struct Column {
        float low  = 0.0F;
        float high = 0.0F;
    };

    explicit EnvelopeHistory(std::size_t columns = 512);

    /// What push() is counted in. Starts over when it changes.
    void setSampleRate(double rate);

    /// Adds samples, newest last.
    void push(const float* samples, std::size_t count);

    /// Silence: time passing with nothing heard, so the envelope scrolls away.
    void pushSilence(std::size_t count);

    void clear();

    [[nodiscard]] std::size_t size() const noexcept { return columns_.size(); }

    /// Column `index`, oldest first, so the last is what was just heard.
    [[nodiscard]] const Column& at(std::size_t index) const {
        return columns_[(next_ + index) % columns_.size()];
    }

private:
    void closeColumn();

    std::vector<Column> columns_;
    std::size_t         next_ = 0;

    double      sampleRate_      = 0.0;
    double      samplesPerColumn_ = 0.0;
    double      filled_          = 0.0;
    Column      open_;
    bool        openHasSamples_  = false;
};

}  // namespace xpcog
