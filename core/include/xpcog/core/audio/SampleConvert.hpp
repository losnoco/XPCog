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

}  // namespace xpcog
