#include "xpcog/core/NaturalOrder.hpp"

#include <cctype>

namespace xpcog {
namespace {

[[nodiscard]] bool isDigit(char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

[[nodiscard]] char fold(char character) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

/// Compares the digit run starting at each position. Returns -1, 0 or 1, and
/// advances both positions past the run.
///
/// Comparing by length after skipping leading zeros, then lexically, handles
/// numbers of any size -- parsing to an integer would overflow on a track named
/// after a catalogue number.
[[nodiscard]] int compareNumbers(std::string_view lhs, std::size_t& i,
                                 std::string_view rhs, std::size_t& j) {
    while (i < lhs.size() && lhs[i] == '0') {
        ++i;
    }
    while (j < rhs.size() && rhs[j] == '0') {
        ++j;
    }

    const std::size_t lhsStart = i;
    const std::size_t rhsStart = j;
    while (i < lhs.size() && isDigit(lhs[i])) {
        ++i;
    }
    while (j < rhs.size() && isDigit(rhs[j])) {
        ++j;
    }

    const std::size_t lhsLength = i - lhsStart;
    const std::size_t rhsLength = j - rhsStart;
    if (lhsLength != rhsLength) {
        return lhsLength < rhsLength ? -1 : 1;
    }

    const std::string_view lhsDigits = lhs.substr(lhsStart, lhsLength);
    const std::string_view rhsDigits = rhs.substr(rhsStart, rhsLength);
    if (lhsDigits != rhsDigits) {
        return lhsDigits < rhsDigits ? -1 : 1;
    }
    return 0;
}

}  // namespace

bool naturalLess(std::string_view lhs, std::string_view rhs) {
    std::size_t i = 0;
    std::size_t j = 0;

    // Remembered rather than acted on: "A2" and "a2" must order consistently,
    // but case must not outrank the rest of the string, or "album/B" would sort
    // before "Album/a".
    int caseTiebreak = 0;

    while (i < lhs.size() && j < rhs.size()) {
        if (isDigit(lhs[i]) && isDigit(rhs[j])) {
            const int order = compareNumbers(lhs, i, rhs, j);
            if (order != 0) {
                return order < 0;
            }
            continue;
        }

        const char a = fold(lhs[i]);
        const char b = fold(rhs[j]);
        if (a != b) {
            return a < b;
        }
        if (caseTiebreak == 0 && lhs[i] != rhs[j]) {
            caseTiebreak = (lhs[i] < rhs[j]) ? -1 : 1;
        }
        ++i;
        ++j;
    }

    if (i < lhs.size() || j < rhs.size()) {
        return j < rhs.size();  // the shorter string sorts first
    }
    return caseTiebreak < 0;
}

}  // namespace xpcog
