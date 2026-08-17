#include "xpcog/core/audio/SampleConvert.hpp"

#include <cstring>

namespace xpcog {
namespace {

/// Cog scales by 2^(bits-1) so full-scale negative lands on exactly -1.0.
/// Note the asymmetry is intentional and matches every other player: +full-scale
/// maps to just under +1.0.
constexpr float kScale8  = 1.0F / 128.0F;
constexpr float kScale16 = 1.0F / 32768.0F;
constexpr float kScale24 = 1.0F / 8388608.0F;
constexpr float kScale32 = 1.0F / 2147483648.0F;

template <typename T>
[[nodiscard]] T loadNative(const std::byte* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

/// Sign-extends a little-endian 24-bit sample into an int32.
[[nodiscard]] std::int32_t load24(const std::byte* p) noexcept {
    const auto b0 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0]));
    const auto b1 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1]));
    const auto b2 = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2]));

    std::uint32_t raw = b0 | (b1 << 8) | (b2 << 16);
    if (raw & 0x800000U) {
        raw |= 0xFF000000U;  // sign-extend
    }
    return static_cast<std::int32_t>(raw);
}

}  // namespace

std::size_t float32SampleCount(const AudioChunk& chunk) noexcept {
    return chunk.frameCount() * chunk.format().channels;
}

std::size_t convertToFloat32(const AudioChunk& chunk, std::span<float> out) noexcept {
    const std::size_t samples = float32SampleCount(chunk);
    if (samples == 0 || out.size() < samples) {
        return 0;
    }

    const std::byte* src = chunk.bytes().data();

    switch (chunk.format().format) {
        case SampleFormat::F32:
            std::memcpy(out.data(), src, samples * sizeof(float));
            break;

        case SampleFormat::F64:
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<float>(loadNative<double>(src + i * 8));
            }
            break;

        case SampleFormat::S8:
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<float>(std::to_integer<std::int8_t>(src[i])) *
                         kScale8;
            }
            break;

        case SampleFormat::U8:
            for (std::size_t i = 0; i < samples; ++i) {
                const auto v = std::to_integer<std::uint8_t>(src[i]);
                out[i] = (static_cast<float>(v) - 128.0F) * kScale8;
            }
            break;

        case SampleFormat::S16:
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<float>(loadNative<std::int16_t>(src + i * 2)) *
                         kScale16;
            }
            break;

        case SampleFormat::S24:
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<float>(load24(src + i * 3)) * kScale24;
            }
            break;

        case SampleFormat::S32:
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] = static_cast<float>(loadNative<std::int32_t>(src + i * 4)) *
                         kScale32;
            }
            break;

        case SampleFormat::DSD:
            // DSD reaches float only via the dsd2pcm decimation filter (M6).
            return 0;
    }

    return samples;
}

}  // namespace xpcog
