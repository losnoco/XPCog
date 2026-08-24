#include "xpcog/core/audio/SampleConvert.hpp"

#include <cmath>
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

namespace {

/// Rounds and clamps one sample to an integer of `bits`.
///
/// In double throughout. The multiply is exact in float32 for anything that came
/// back from convertToFloat32, but the *rounding* is not: 8388607 / 2^23 scaled
/// back is 8388607.0, and adding 0.5 to that in float32 needs a 25th mantissa
/// bit, so it becomes 8388608.0 and clamps to 8388606 -- an off-by-one at
/// exactly the extreme where a DoP marker lives.
[[nodiscard]] std::int32_t quantise(float sample, int bits) noexcept {
    const double scale   = static_cast<double>(std::int64_t{1} << (bits - 1));
    const double maximum = scale - 1.0;
    // std::round rather than std::lround, which returns `long` -- 32 bits on
    // Windows, so rounding a full-scale S32 sample overflows it before the clamp
    // below ever runs. Staying in double until after the clamp has no such edge.
    const double scaled = std::round(static_cast<double>(sample) * scale);
    if (scaled >= maximum) {
        return static_cast<std::int32_t>(maximum);
    }
    if (scaled <= -scale) {
        return static_cast<std::int32_t>(-scale);
    }
    return static_cast<std::int32_t>(scaled);
}

void store24(std::byte* p, std::int32_t value) noexcept {
    const auto raw = static_cast<std::uint32_t>(value);
    p[0]           = static_cast<std::byte>(raw & 0xFFU);
    p[1]           = static_cast<std::byte>((raw >> 8) & 0xFFU);
    p[2]           = static_cast<std::byte>((raw >> 16) & 0xFFU);
}

template <typename T>
void storeNative(std::byte* p, T value) noexcept {
    std::memcpy(p, &value, sizeof(T));
}

}  // namespace

std::size_t packedByteCount(std::size_t sampleCount, SampleFormat format) noexcept {
    switch (format) {
        case SampleFormat::S16: return sampleCount * 2;
        case SampleFormat::S24: return sampleCount * 3;
        case SampleFormat::S32:
        case SampleFormat::F32: return sampleCount * 4;
        default: return 0;
    }
}

std::size_t convertFromFloat32(std::span<const float> in, SampleFormat format,
                               std::span<std::byte> out) noexcept {
    const std::size_t needed = packedByteCount(in.size(), format);
    if (needed == 0 || out.size() < needed) {
        return 0;
    }

    std::byte* p = out.data();
    switch (format) {
        case SampleFormat::F32:
            // Straight through: no scaling, no rounding, nothing to get wrong.
            std::memcpy(p, in.data(), needed);
            break;
        case SampleFormat::S16:
            for (const float sample : in) {
                storeNative<std::int16_t>(p, static_cast<std::int16_t>(quantise(sample, 16)));
                p += 2;
            }
            break;
        case SampleFormat::S24:
            for (const float sample : in) {
                store24(p, quantise(sample, 24));
                p += 3;
            }
            break;
        case SampleFormat::S32:
            for (const float sample : in) {
                storeNative<std::int32_t>(p, quantise(sample, 32));
                p += 4;
            }
            break;
        default: return 0;
    }
    return needed;
}

}  // namespace xpcog
