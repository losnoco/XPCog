// SHA-256, used to content-address album artwork.
//
// Cog reaches CommonCrypto through a `SHA256Digest` Swift class
// (PlaylistEntry.m:447). There is no cross-platform equivalent worth a
// dependency for one hash, and the algorithm is small enough to carry.
//
// This is a digest for deduplication, not a security primitive: it is not
// constant-time and has no HMAC.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace xpcog {

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data);

/// Lowercase hex, 64 characters. The form Cog stores in `artHash`.
[[nodiscard]] std::string sha256Hex(std::span<const std::byte> data);

}  // namespace xpcog
