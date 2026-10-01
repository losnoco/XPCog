#include "UrlHistory.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace xpcog::app {
namespace {

/// Cog's kMaximumURLs.
constexpr std::size_t kMaxHistory = 15;

[[nodiscard]] std::string trim(std::string_view text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    while (begin < text.size() && space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}


}  // namespace

std::string trimUrl(std::string_view text) { return trim(text); }

std::vector<std::string> urlHistoryFrom(const std::string& stored) {
    std::vector<std::string> entries;
    std::size_t              start = 0;
    for (;;) {
        const std::size_t newline = stored.find('\n', start);
        const std::string line =
            trim(newline == std::string::npos ? std::string_view{stored}.substr(start)
                                              : std::string_view{stored}.substr(
                                                    start, newline - start));
        if (!line.empty()) {
            entries.push_back(line);
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return entries;
}

std::vector<std::string> urlHistoryWith(std::vector<std::string> history,
                                        const std::string&       url) {
    history.erase(std::remove(history.begin(), history.end(), url), history.end());
    history.push_back(url);
    while (history.size() > kMaxHistory) {
        history.erase(history.begin());
    }
    return history;
}

std::string joinUrlHistory(const std::vector<std::string>& history) {
    std::string joined;
    for (const std::string& entry : history) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += entry;
    }
    return joined;
}

}  // namespace xpcog::app
