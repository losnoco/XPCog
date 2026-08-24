// Integer/float sample conversion.
//
// In Cog this lives inside ChunkList (Audio/Chain/ChunkList.m, 1113 lines, which
// is both the FIFO and the format converter). Splitting it out keeps the
// conversion independently testable. ChunkList lands in M1b/M1c and will call
// these; DSD and HDCD join them in M1c/M6.

#pragma once

#include "xpcog/core/AudioChunk.hpp"

#include <cstddef>
#include <span>

namespace xpcog {

/// Converts interleaved samples of any supported integer or float layout to
/// float32 in [-1, 1). Returns the number of samples written, which is
/// `frames * channels` when `out` is large enough and 0 otherwise.
///
/// Scaling matches Cog: divide by 2^(containerBits-1), so a full-scale negative
/// sample maps to exactly -1.0 and full-scale positive to just under +1.0.
std::size_t convertToFloat32(const AudioChunk& chunk, std::span<float> out) noexcept;

/// Samples that `chunk` will produce: frameCount * channels.
[[nodiscard]] std::size_t float32SampleCount(const AudioChunk& chunk) noexcept;

/// The other direction: interleaved float32 to packed little-endian samples of
/// `format`, for handing a device something other than float.
///
/// Returns bytes written, or 0 when `out` is too small or `format` is not one a
/// device is opened in (S16, S24, S32 and F32 are; U8, S8, F64 and DSD are not).
///
/// **Exactly the inverse of convertToFloat32, and exact is meant literally.**
/// The scale is the same 2^(bits-1), and the rounding is done in double so that
/// a float which arrived as `n / 2^23` comes back as `n` for every n a 24-bit
/// sample can hold -- including the extremes, where doing the arithmetic in
/// float32 rounds 8388607.5 to 8388608 and clamps to the wrong answer.
///
/// That matters because of what wants an integer device in the first place. DoP
/// carries DSD inside PCM as a 0x05/0xFA marker byte and a payload, and every
/// one of those bytes has to reach the DAC unaltered or it sees noise instead of
/// a DSD stream. So: no dither, and no scaling that cannot be undone. Cog says
/// the same thing by construction -- its DoP path hands CoreAudio integers
/// directly (ChunkList.m, convert_dsd_to_dop_f32 plus an integer render format).
///
/// Values outside [-1, 1) clamp rather than wrap. Wrapping turns one loud sample
/// into a full-scale click of the opposite sign.
std::size_t convertFromFloat32(std::span<const float> in, SampleFormat format,
                               std::span<std::byte> out) noexcept;

/// Bytes convertFromFloat32 needs for `sampleCount` samples of `format`, or 0
/// for a format it does not write.
[[nodiscard]] std::size_t packedByteCount(std::size_t sampleCount,
                                          SampleFormat format) noexcept;

}  // namespace xpcog
