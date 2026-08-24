// MD5, used to sign Last.fm API calls.
//
// Carried for the same reason as Sha256 next door: one hash is not worth a
// dependency, and the algorithm is small. Cog reaches CommonCrypto for this
// through an `NSData.md5()` category (Scrobbler/LastFMAPI.swift:115).
//
// **This is not a security primitive and must not be used as one.** MD5 is
// collision-broken and has been since 2004. It is here because Last.fm's
// `api_sig` is specified as MD5 and a client does not get to pick the hash --
// the signature is a shared-secret authenticator against a server that computes
// the same digest, not a claim that the digest is strong. Nothing else in this
// tree should call it: artwork is content-addressed with SHA-256, which is what
// Sha256.hpp is for.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace xpcog {

using Md5Digest = std::array<std::uint8_t, 16>;

[[nodiscard]] Md5Digest md5(std::span<const std::byte> data);

/// Lowercase hex, 32 characters. The form Last.fm's `api_sig` takes.
[[nodiscard]] std::string md5Hex(std::span<const std::byte> data);

/// Convenience for the only caller: signing a string of concatenated parameters.
[[nodiscard]] std::string md5Hex(std::string_view text);

}  // namespace xpcog
