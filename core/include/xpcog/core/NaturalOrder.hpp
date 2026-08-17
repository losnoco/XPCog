// Human-friendly string ordering, so "track 9" sorts before "track 10".
//
// Cog uses -finderCompare: (Utils/NSString+FinderCompare.m), which delegates to
// NSString's localizedStandardCompare -- Unicode-aware and locale-sensitive. The
// app layer gets that behaviour back from QCollator with numeric mode on.
//
// This is the Qt-free stand-in that core needs for one job: the order files come
// out of a directory scan. Getting that wrong puts track 10 second in the
// playlist, which is immediately visible. It is ASCII case-folding and digit-run
// comparison, not collation -- deliberately, because guessing at locale rules
// without a collation library produces orderings that are wrong in a subtler way
// than plain byte order.

#pragma once

#include <string_view>

namespace xpcog {

/// True when `lhs` sorts before `rhs`. Digit runs compare as numbers, of any
/// length, and letters compare case-insensitively with case as the tiebreak so
/// the order stays total.
[[nodiscard]] bool naturalLess(std::string_view lhs, std::string_view rhs);

}  // namespace xpcog
