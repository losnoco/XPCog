// The Open URL dialog's history: Cog's kMaximumURLs most recent addresses,
// newline-separated in one setting, most recent last.

#pragma once

#include <string>
#include <vector>

namespace xpcog::app {

/// The entries in `stored`, one per line, trimmed, blanks dropped.
[[nodiscard]] std::vector<std::string> urlHistoryFrom(const std::string& stored);

/// `history` with `url` moved or added to the end, and the oldest dropped
/// past fifteen -- Cog's kMaximumURLs.
[[nodiscard]] std::vector<std::string> urlHistoryWith(std::vector<std::string> history,
                                                      const std::string&       url);

/// The setting's spelling of `history`.
[[nodiscard]] std::string joinUrlHistory(const std::vector<std::string>& history);

/// `text` without the whitespace either end, which is what an address typed
/// into a box has more often than not.
[[nodiscard]] std::string trimUrl(std::string_view text);

}  // namespace xpcog::app
