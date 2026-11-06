#include "xpcog/core/audio/EnvelopeHistory.hpp"

#include <algorithm>

namespace xpcog {

EnvelopeHistory::EnvelopeHistory(std::size_t columns) : columns_(std::max<std::size_t>(columns, 1)) {}

void EnvelopeHistory::setSampleRate(double rate) {
    if (rate == sampleRate_) {
        return;
    }
    sampleRate_       = rate;
    samplesPerColumn_ = rate * kSeconds / static_cast<double>(columns_.size());
    clear();
}

void EnvelopeHistory::clear() {
    std::fill(columns_.begin(), columns_.end(), Column{});
    next_           = 0;
    filled_         = 0.0;
    open_           = Column{};
    openHasSamples_ = false;
}

void EnvelopeHistory::closeColumn() {
    columns_[next_] = open_;
    next_           = (next_ + 1) % columns_.size();
    filled_ -= samplesPerColumn_;
    open_           = Column{};
    openHasSamples_ = false;
}

void EnvelopeHistory::push(const float* samples, std::size_t count) {
    if (samplesPerColumn_ <= 0.0) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float sample = samples[i];
        if (openHasSamples_) {
            open_.low  = std::min(open_.low, sample);
            open_.high = std::max(open_.high, sample);
        } else {
            open_           = Column{sample, sample};
            openHasSamples_ = true;
        }
        filled_ += 1.0;
        if (filled_ >= samplesPerColumn_) {
            closeColumn();
        }
    }
}

void EnvelopeHistory::pushSilence(std::size_t count) {
    if (samplesPerColumn_ <= 0.0) {
        return;
    }
    filled_ += static_cast<double>(count);
    while (filled_ >= samplesPerColumn_) {
        closeColumn();
    }
}

}  // namespace xpcog
