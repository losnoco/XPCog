// Text handling shared by the playlist containers (M3U, PLS, and later XSPF).

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <string>
#include <vector>

namespace xpcog::codecs {

/// Reads `source` to the end and normalises line endings, so CR, LF and CRLF all
/// yield the same lines. Cog rewrites '\r' to '\n' in place for the same reason.
///
/// Encoding is TextEncoding.hpp's rule -- valid UTF-8 as-is, anything else
/// Latin-1. Cog additionally tries GB18030 and CP1251 before falling back, which
/// needs a full conversion library; Latin-1 never fails, so no line is ever lost
/// -- a mis-decoded non-UTF-8 path simply will not resolve, which is the same
/// outcome Cog reaches when its guesses are wrong too.
[[nodiscard]] std::string readAllText(ISource& source);

/// Splits on '\n' and trims surrounding whitespace, dropping empty lines.
[[nodiscard]] std::vector<std::string> splitLines(const std::string& text);

/// Resolves one playlist entry against the playlist's own location.
///
/// Handles what Cog's +urlForPath:relativeTo: handles: absolute URLs pass
/// through, Windows backslashes become forward slashes, relative paths resolve
/// against the playlist's directory, and a trailing "#123" is preserved as a
/// fragment rather than being treated as part of the filename -- that fragment
/// carries the cue-sheet track or subsong index.
[[nodiscard]] Url resolveEntry(const std::string& entry, const Url& playlistUrl);

}  // namespace xpcog::codecs
