#include "xpcog/core/audio/Oscilloscope.hpp"

#include <algorithm>
#include <cmath>

namespace xpcog {

float scopeLevel(float sample, float gain, ScopeScale scale) noexcept {
    const float linear = std::clamp(sample * gain, -1.0F, 1.0F);
    if (scale == ScopeScale::Linear || linear == 0.0F) {
        return linear;
    }
    const float magnitude = std::fabs(linear);
    const float db        = 20.0F * std::log10(magnitude);
    const float height    = std::clamp(1.0F - (db / kScopeLogFloorDb), 0.0F, 1.0F);
    return std::copysign(height, linear);
}

std::size_t triggerOffset(std::span<const float> samples, std::size_t window) noexcept {
    if (window == 0 || samples.size() < window) {
        return 0;
    }
    const std::size_t newest = samples.size() - window;
    const std::size_t from   = samples.size() >= 2 * window ? samples.size() - (2 * window) : 0;

    // From the oldest candidate forward, so the trace starts at the same phase
    // whichever way the window drifted since the last frame. Armed by a sample
    // below the band, fired by the first at or above it: a plain "previous
    // below, this above" misses every crossing a slow signal makes, since it
    // spends several samples inside the band on the way through.
    bool armed = false;
    for (std::size_t i = from; i <= newest; ++i) {
        const float sample = samples[i];
        if (sample < -kTriggerHysteresis) {
            armed = true;
        } else if (armed && sample >= kTriggerHysteresis) {
            return i;
        }
    }
    return newest;
}

void foldForDisplay(std::span<const float> samples, float gain, ScopeScale scale,
                    std::span<std::pair<float, float>> columns) noexcept {
    if (columns.empty()) {
        return;
    }
    if (samples.empty()) {
        std::fill(columns.begin(), columns.end(), std::pair{0.0F, 0.0F});
        return;
    }

    const auto scaled = [gain, scale](float sample) { return scopeLevel(sample, gain, scale); };

    const std::size_t count = samples.size();
    const std::size_t width = columns.size();

    if (count <= width) {
        // Fewer samples than columns: nearest sample, no band.
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t index = std::min(count - 1, (x * count) / width);
            const float       value = scaled(samples[index]);
            columns[x] = {value, value};
        }
        return;
    }

    for (std::size_t x = 0; x < width; ++x) {
        const std::size_t first = (x * count) / width;
        const std::size_t last  = std::max(first + 1, ((x + 1) * count) / width);
        float             low   = scaled(samples[first]);
        float             high  = low;
        for (std::size_t i = first + 1; i < last && i < count; ++i) {
            const float value = scaled(samples[i]);
            low  = std::min(low, value);
            high = std::max(high, value);
        }
        columns[x] = {low, high};
    }
}

}  // namespace xpcog
