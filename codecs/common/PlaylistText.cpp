#include "PlaylistText.hpp"

#include "common/TextEncoding.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace xpcog::codecs {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::find_if(text.begin(), text.end(), notSpace);
    const auto end   = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    return (begin < end) ? std::string{begin, end} : std::string{};
}

}  // namespace

std::string readAllText(ISource& source) {
    std::string       text;
    char              buffer[4096];
    std::int64_t      got = 0;
    while ((got = source.read(buffer, sizeof(buffer))) > 0) {
        text.append(buffer, static_cast<std::size_t>(got));
    }

    // Strip a UTF-8 BOM; leaving it makes the first entry unresolvable.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    text = toUtf8(std::move(text));

    std::replace(text.begin(), text.end(), '\r', '\n');
    return text;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t              start = 0;

    while (start <= text.size()) {
        const std::size_t end  = text.find('\n', start);
        const std::size_t stop = (end == std::string::npos) ? text.size() : end;

        std::string line = trimmed(std::string_view{text}.substr(start, stop - start));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

Url resolveEntry(const std::string& entry, const Url& playlistUrl) {
    // Already a URL? Take it verbatim.
    if (entry.find("://") != std::string::npos) {
        if (auto parsed = Url::parse(entry)) {
            return *parsed;
        }
    }

    std::string path = entry;
    std::string fragment;

    // A trailing "#<digits>" is a subsong or cue-track index, not part of the
    // filename. Cog scans for exactly this pattern; anything else after '#' is
    // left alone, since '#' is legal in a filename.
    if (const std::size_t hash = path.rfind('#');
        hash != std::string::npos && hash + 1 < path.size()) {
        const std::string_view tail{path};
        if (std::all_of(tail.begin() + static_cast<std::ptrdiff_t>(hash) + 1, tail.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            fragment = path.substr(hash + 1);
            path.erase(hash);
        }
    }

    // Playlists written on Windows use backslashes.
    std::replace(path.begin(), path.end(), '\\', '/');

    std::filesystem::path resolved{path};
    if (resolved.is_relative()) {
        if (const auto base = playlistUrl.localPath()) {
            resolved = base->parent_path() / resolved;
        }
    }

    Url url = Url::fromLocalPath(resolved);
    return fragment.empty() ? url : url.withFragment(fragment);
}

}  // namespace xpcog::codecs
