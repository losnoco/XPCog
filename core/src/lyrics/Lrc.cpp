#include "xpcog/core/lyrics/Lrc.hpp"

#include "xpcog/core/Utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>

namespace xpcog {
namespace {

/// Larger than any lyrics file, by a wide margin: a long song's LRC is a few
/// kilobytes. Something bigger named `.lrc` is not lyrics, and reading it on
/// the interface thread to find that out is not worth it.
constexpr std::uintmax_t kMaxSidecarBytes = 1024 * 1024;

[[nodiscard]] bool isDigit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

/// Reads a run of digits at `pos`, advancing past it. False when there is none.
bool readNumber(std::string_view text, std::size_t& pos, std::int64_t& value,
                std::size_t* digits = nullptr) {
    const std::size_t start = pos;
    value                   = 0;
    // Nine digits is far past any real minute count, and stops a line of
    // digits in brackets overflowing into a nonsense time.
    while (pos < text.size() && isDigit(text[pos]) && pos - start < 9) {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    if (digits != nullptr) {
        *digits = pos - start;
    }
    return pos > start;
}

/// `mm:ss`, `mm:ss.f+` or `mm:ss:ff` -- the inside of a stamp, whole. Seconds.
[[nodiscard]] std::optional<double> parseStamp(std::string_view inside) {
    std::size_t  pos = 0;
    std::int64_t minutes = 0;
    std::int64_t seconds = 0;
    if (!readNumber(inside, pos, minutes) || pos >= inside.size() || inside[pos] != ':') {
        return std::nullopt;
    }
    ++pos;
    std::size_t secondDigits = 0;
    if (!readNumber(inside, pos, seconds, &secondDigits) || secondDigits > 2 || seconds > 59) {
        return std::nullopt;
    }
    double fraction = 0.0;
    if (pos < inside.size() && (inside[pos] == '.' || inside[pos] == ':')) {
        ++pos;
        std::int64_t digitsValue = 0;
        std::size_t  digits      = 0;
        if (!readNumber(inside, pos, digitsValue, &digits)) {
            return std::nullopt;
        }
        double scale = 1.0;
        for (std::size_t i = 0; i < digits; ++i) {
            scale *= 10.0;
        }
        fraction = static_cast<double>(digitsValue) / scale;
    }
    if (pos != inside.size()) {
        return std::nullopt;
    }
    return static_cast<double>(minutes) * 60.0 + static_cast<double>(seconds) + fraction;
}

/// Drops enhanced LRC's `<mm:ss.xx>` word stamps from a line's words.
[[nodiscard]] std::string stripWordStamps(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '<') {
            const auto close = text.find('>', pos + 1);
            if (close != std::string_view::npos &&
                parseStamp(text.substr(pos + 1, close - pos - 1))) {
                pos = close + 1;
                continue;
            }
        }
        out.push_back(text[pos]);
        ++pos;
    }
    return out;
}

/// `[offset:+500]` and friends: a header, not a stamp. Returns the tag name,
/// lowercased, and the value; nullopt if `line` is not one.
[[nodiscard]] std::optional<std::pair<std::string, std::string_view>>
parseHeader(std::string_view line) {
    if (line.size() < 3 || line.front() != '[' || line.back() != ']') {
        return std::nullopt;
    }
    const std::string_view inside = line.substr(1, line.size() - 2);
    const auto             colon  = inside.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    std::string name;
    for (const char c : inside.substr(0, colon)) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter) {
            return std::nullopt;
        }
        name.push_back(static_cast<char>(c | 0x20));
    }
    return std::make_pair(std::move(name), trim(inside.substr(colon + 1)));
}

[[nodiscard]] std::optional<std::int64_t> parseOffset(std::string_view value) {
    bool        negative = false;
    std::size_t pos      = 0;
    if (!value.empty() && (value[0] == '+' || value[0] == '-')) {
        negative = value[0] == '-';
        pos      = 1;
    }
    std::int64_t magnitude = 0;
    if (!readNumber(value, pos, magnitude) || pos != value.size()) {
        return std::nullopt;
    }
    return negative ? -magnitude : magnitude;
}

[[nodiscard]] std::string latin1ToUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            out.push_back(c);
        } else {
            out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return out;
}

}  // namespace

std::size_t SyncedLyrics::lineAt(double seconds) const {
    // upper_bound on time: the first line not yet reached. The one before it
    // is the one being sung.
    const auto next = std::upper_bound(
        lines.begin(), lines.end(), seconds,
        [](double at, const LrcLine& line) { return at < line.time; });
    if (next == lines.begin()) {
        return npos;
    }
    return static_cast<std::size_t>(std::distance(lines.begin(), next)) - 1;
}

std::string SyncedLyrics::plainText() const {
    std::size_t first = 0;
    std::size_t last  = lines.size();
    while (first < last && lines[first].text.empty()) {
        ++first;
    }
    while (last > first && lines[last - 1].text.empty()) {
        --last;
    }
    std::string out;
    for (std::size_t i = first; i < last; ++i) {
        if (i > first) {
            out.push_back('\n');
        }
        out += lines[i].text;
    }
    return out;
}

std::optional<SyncedLyrics> parseLrc(std::string_view text) {
    SyncedLyrics result;
    std::int64_t offsetMs = 0;
    std::size_t  timed    = 0;
    std::size_t  untimed  = 0;

    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trim(text.substr(start, end - start));
        // CRLF is one break, not two: skip the LF of the pair.
        start = end + ((end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n') ? 2 : 1);

        if (line.empty()) {
            continue;
        }

        // Every stamp at the front of the line, however many there are.
        std::vector<double> times;
        std::size_t         pos = 0;
        while (pos < line.size() && line[pos] == '[') {
            const auto close = line.find(']', pos + 1);
            if (close == std::string_view::npos) {
                break;
            }
            const auto time = parseStamp(line.substr(pos + 1, close - pos - 1));
            if (!time) {
                break;
            }
            times.push_back(*time);
            pos = close + 1;
        }

        if (times.empty()) {
            if (const auto header = parseHeader(line)) {
                if (header->first == "offset") {
                    offsetMs = parseOffset(header->second).value_or(offsetMs);
                }
                continue;
            }
            ++untimed;
            continue;
        }

        ++timed;
        std::string words{trim(stripWordStamps(line.substr(pos)))};
        for (const double time : times) {
            result.lines.push_back(LrcLine{time, words});
        }
    }

    if (timed == 0 || timed <= untimed) {
        return std::nullopt;
    }

    // The offset applies to the whole file wherever the header sat, which is
    // why it is applied after the read rather than to the stamps that follow it.
    const double shift = static_cast<double>(offsetMs) / 1000.0;
    for (auto& line : result.lines) {
        line.time = std::max(0.0, line.time - shift);
    }
    std::stable_sort(result.lines.begin(), result.lines.end(),
                     [](const LrcLine& a, const LrcLine& b) { return a.time < b.time; });
    return result;
}

std::optional<std::string> readSidecarLrc(const Url& url) {
    if (!url.fragment().empty()) {
        return std::nullopt;
    }
    const auto local = url.localPath();
    if (!local) {
        return std::nullopt;
    }
    std::filesystem::path sidecar = *local;
    sidecar.replace_extension(".lrc");
    if (sidecar == *local) {
        return std::nullopt;  // the track *is* an .lrc; nothing sits beside it
    }

    std::error_code ec;
    const auto      size = std::filesystem::file_size(sidecar, ec);
    if (ec || size == 0 || size > kMaxSidecarBytes) {
        return std::nullopt;
    }
    std::ifstream in(sidecar, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    if (!isValidUtf8(bytes)) {
        return latin1ToUtf8(bytes);
    }
    return bytes;
}

}  // namespace xpcog
