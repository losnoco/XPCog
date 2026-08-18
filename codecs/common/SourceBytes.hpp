// Reading a source whole.
//
// Some decoders do not stream: libopenmpt and libgme both parse a module or a
// music rip into memory up front and synthesise from it afterwards, so there is
// no incremental read to do. They want the bytes, once.
//
// Not readAllText()'s binary sibling by accident -- that one normalises line
// endings and guesses an encoding, which for a module file would corrupt it.

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace xpcog::codecs {

/// Enough for any module or music rip, and small enough that a wrong guess
/// about what is at the end of a URL cannot exhaust memory. Cog reads whatever
/// -tell reports after seeking to the end, with no cap at all.
inline constexpr std::size_t kDefaultReadLimit = 512U * 1024U * 1024U;

/// Reads `source` to the end. Returns nullopt if the source errors partway, so
/// a truncated read is never mistaken for a short file -- a decoder handed half
/// a module fails in ways that look like a corrupt file rather than a dropped
/// connection.
///
/// Reads until EOF rather than seeking to the end for a size: that works on a
/// non-seekable source too, which is what an HTTP stream is.
[[nodiscard]] std::optional<std::vector<std::byte>> readAllBytes(
    ISource& source, std::size_t limit = kDefaultReadLimit);

}  // namespace xpcog::codecs
