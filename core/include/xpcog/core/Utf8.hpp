// Is this UTF-8?
//
// The question comes up wherever bytes arrive without a charset: a playlist file,
// an ICY header, a percent-encoded URL written by an older build. Answering it is
// what makes "valid UTF-8 as-is, otherwise fall back" a safe rule -- valid UTF-8
// is self-identifying, so a fallback only ever fires on input that could not have
// been UTF-8 in the first place.
//
// In core rather than codecs because both halves of the port need it and neither
// should own it: xpcog::codecs::isValidUtf8 forwards here.

#pragma once

#include <string_view>

namespace xpcog {

/// Strict: rejects overlong-looking lead bytes, truncated sequences and stray
/// continuation bytes. Deliberately not a full decoder -- the question is only
/// whether to trust the bytes or reinterpret them.
[[nodiscard]] bool isValidUtf8(std::string_view text);

}  // namespace xpcog
