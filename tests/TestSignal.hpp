// Shared constants for the synthetic signals the tests generate.
//
// M_PI is not standard C++ and MSVC does not define it without
// _USE_MATH_DEFINES, so every test that generated a sine broke the Windows
// build. Defining the macro would work; declaring the constant is what the
// language actually offers.

#pragma once

#include <cstddef>
#include <span>

namespace xpcog::test {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

/// The dominant frequency of a mono span, by zero crossings over its steady
/// middle. Crude, and deliberately so: it is immune to the codec delay and the
/// leading padding that make a phase-sensitive comparison useless, while still
/// telling one tone from another -- which is all a test needs to know it decoded
/// the track it asked for rather than the one beside it.
[[nodiscard]] inline double dominantFrequency(std::span<const float> mono,
                                              double                 sampleRate) {
    if (mono.size() < 1024) {
        return 0.0;
    }
    const std::size_t begin = mono.size() / 4;
    const std::size_t end   = mono.size() * 3 / 4;

    std::size_t crossings = 0;
    float       previous  = 0.0F;
    for (std::size_t i = begin; i < end; ++i) {
        if (i > begin && ((previous < 0.0F) != (mono[i] < 0.0F))) {
            ++crossings;
        }
        previous = mono[i];
    }
    return static_cast<double>(crossings) * sampleRate /
           (2.0 * static_cast<double>(end - begin));
}

}  // namespace xpcog::test
