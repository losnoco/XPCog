// Replaces Cog's AudioStreamBasicDescription usage.
//
// The channel bitmask and its helpers are ported verbatim from Cog
// Audio/Chain/AudioChunk.h and AudioChunk.m. The enum values are kept
// byte-identical: they are persisted in the library database and compared
// against decoder output, so renumbering them would silently corrupt channel
// mapping for existing playlists.

#pragma once

#include <bit>
#include <cstdint>

namespace xpcog {

enum ChannelFlag : std::uint32_t {
    kChannelFrontLeft        = 1U << 0,
    kChannelFrontRight       = 1U << 1,
    kChannelFrontCenter      = 1U << 2,
    kChannelLFE              = 1U << 3,
    kChannelBackLeft         = 1U << 4,
    kChannelBackRight        = 1U << 5,
    kChannelFrontCenterLeft  = 1U << 6,
    kChannelFrontCenterRight = 1U << 7,
    kChannelBackCenter       = 1U << 8,
    kChannelSideLeft         = 1U << 9,
    kChannelSideRight        = 1U << 10,
    kChannelTopCenter        = 1U << 11,
    kChannelTopFrontLeft     = 1U << 12,
    kChannelTopFrontCenter   = 1U << 13,
    kChannelTopFrontRight    = 1U << 14,
    kChannelTopBackLeft      = 1U << 15,
    kChannelTopBackCenter    = 1U << 16,
    kChannelTopBackRight     = 1U << 17,
};

enum ChannelConfig : std::uint32_t {
    kConfigMono   = kChannelFrontCenter,
    kConfigStereo = kChannelFrontLeft | kChannelFrontRight,
    kConfig3Point0 =
        kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter,
    kConfig4Point0 =
        kChannelFrontLeft | kChannelFrontRight | kChannelBackLeft | kChannelBackRight,
    kConfig5Point0 = kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter |
                     kChannelBackLeft | kChannelBackRight,
    kConfig5Point1 = kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter |
                     kChannelLFE | kChannelBackLeft | kChannelBackRight,
    kConfig5Point1Side = kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter |
                         kChannelLFE | kChannelSideLeft | kChannelSideRight,
    kConfig6Point1 = kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter |
                     kChannelLFE | kChannelBackCenter | kChannelSideLeft |
                     kChannelSideRight,
    kConfig7Point1 = kChannelFrontLeft | kChannelFrontRight | kChannelFrontCenter |
                     kChannelLFE | kChannelBackLeft | kChannelBackRight |
                     kChannelSideLeft | kChannelSideRight,
};

/// Sample layout of a buffer. `DSD` is carried through the chain in M6.
enum class SampleFormat : std::uint8_t { U8, S8, S16, S24, S32, F32, F64, DSD };

struct AudioFormat {
    double        sampleRate    = 0.0;
    std::uint32_t channels      = 0;
    std::uint32_t channelConfig = 0;
    SampleFormat  format        = SampleFormat::F32;
    /// Meaningful bits, which can be fewer than the container: 20-bit in S24,
    /// 24-bit in S32. Cog tracks this to report source resolution accurately.
    std::uint32_t bitsPerSample = 32;
    bool          bigEndian     = false;

    [[nodiscard]] constexpr bool isFloat() const noexcept {
        return format == SampleFormat::F32 || format == SampleFormat::F64;
    }

    [[nodiscard]] constexpr std::uint32_t bytesPerSample() const noexcept {
        switch (format) {
            case SampleFormat::U8:
            case SampleFormat::S8:
            case SampleFormat::DSD: return 1;
            case SampleFormat::S16: return 2;
            case SampleFormat::S24: return 3;
            case SampleFormat::S32:
            case SampleFormat::F32: return 4;
            case SampleFormat::F64: return 8;
        }
        return 0;
    }

    [[nodiscard]] constexpr std::uint32_t bytesPerFrame() const noexcept {
        return bytesPerSample() * channels;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return sampleRate > 0.0 && channels > 0 && bytesPerFrame() > 0;
    }

    [[nodiscard]] friend constexpr bool operator==(const AudioFormat&,
                                                   const AudioFormat&) = default;
};

// --- Channel-config helpers, ported from Cog AudioChunk.m ------------------

/// Number of set channel bits. Cog: +countChannels:
[[nodiscard]] constexpr std::uint32_t countChannels(std::uint32_t config) noexcept {
    return static_cast<std::uint32_t>(std::popcount(config));
}

/// Default layout for a bare channel count. Cog: +guessChannelConfig:
[[nodiscard]] std::uint32_t guessChannelConfig(std::uint32_t channelCount) noexcept;

/// Position of `flag` within `config`, or ~0 if absent.
/// Cog: +channelIndexFromConfig:forFlag:
[[nodiscard]] std::uint32_t channelIndexFromConfig(std::uint32_t config,
                                                   std::uint32_t flag) noexcept;

/// The `index`-th set flag in `config`, or 0 if there are fewer.
/// Cog: +extractChannelFlag:fromConfig:
[[nodiscard]] std::uint32_t extractChannelFlag(std::uint32_t index,
                                               std::uint32_t config) noexcept;

/// Bit position of a single-bit `flag`. Cog: +findChannelIndex:
[[nodiscard]] std::uint32_t findChannelIndex(std::uint32_t flag) noexcept;

}  // namespace xpcog
