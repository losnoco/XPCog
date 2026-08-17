#include "xpcog/core/AudioFormat.hpp"

#include <array>
#include <cassert>

namespace xpcog {
namespace {

// Cog Audio/Chain/AudioChunk.m:61. Index is the channel count. The gaps (0 and 9)
// are intentional: Cog falls back to a plain low-bits mask for those counts.
constexpr std::array<std::uint32_t, 11> kChannelConfigTable = {
    0,
    kConfigMono,
    kConfigStereo,
    kConfig3Point0,
    kConfig4Point0,
    kConfig5Point0,
    kConfig5Point1,
    kConfig6Point1,
    kConfig7Point1,
    0,
    // Cast because these are two distinct enum types, and C++20 deprecates a
    // bitwise operation between them. The values are Cog's 18-bit mask and must
    // stay byte-identical, so widening the enums is not an option.
    static_cast<std::uint32_t>(kConfig7Point1) |
        static_cast<std::uint32_t>(kChannelFrontCenterLeft) |
        static_cast<std::uint32_t>(kChannelFrontCenterRight),
};

}  // namespace

std::uint32_t guessChannelConfig(std::uint32_t channelCount) noexcept {
    if (channelCount == 0 || channelCount > 32) {
        return 0;
    }

    std::uint32_t config = 0;
    if (channelCount < kChannelConfigTable.size()) {
        config = kChannelConfigTable[channelCount];
    }
    if (config == 0) {
        // Guard the UB at channelCount == 32, which Cog's (1 << count) - 1 hits.
        config = (channelCount >= 32) ? 0xFFFFFFFFU : ((1U << channelCount) - 1U);
    }

    assert(countChannels(config) == channelCount);
    return config;
}

std::uint32_t channelIndexFromConfig(std::uint32_t config, std::uint32_t flag) noexcept {
    std::uint32_t index = 0;
    for (std::uint32_t walk = 0; walk < 32; ++walk) {
        const std::uint32_t query = 1U << walk;
        if (flag & query) {
            return index;
        }
        if (config & query) {
            ++index;
        }
    }
    return ~0U;
}

std::uint32_t extractChannelFlag(std::uint32_t index, std::uint32_t config) noexcept {
    std::uint32_t toSkip = index;
    std::uint32_t flag   = 1;
    while (flag) {
        if (config & flag) {
            if (toSkip == 0) {
                break;
            }
            --toSkip;
        }
        flag <<= 1;
    }
    return flag;
}

std::uint32_t findChannelIndex(std::uint32_t flag) noexcept {
    assert(flag != 0 && "findChannelIndex requires a set bit");
    return static_cast<std::uint32_t>(std::countr_zero(flag));
}

}  // namespace xpcog
