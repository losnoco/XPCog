// M3U and PLS playlist containers.
// Ports of Cog Plugins/M3u/M3uContainer.m and Plugins/Pls/PlsContainer.m.

#include "../common/PlaylistText.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

/// True for a Game_Music_Emu sidecar line: `rip.nsf::NSF,1,Track 1,0:00:14,,`.
///
/// These sit beside a multi-track rip carrying its per-track names and lengths,
/// and they wear the .m3u extension without being playlists -- the GME decoder
/// hands the whole file to gme_load_m3u_data() instead. Read as a playlist each
/// line becomes a path that cannot exist, so a folder of rips fills the playlist
/// with dead entries. Cog has the same collision and the same result.
///
/// Matched on the shape rather than on `::` alone, because `::` is also how an
/// IPv6 host is written: `http://[::1]/x.mp3` is a real entry. A GME line always
/// names its emulator type between the `::` and the first comma, and that token
/// is plain alphanumerics.
[[nodiscard]] bool isGameMusicSidecarLine(std::string_view line) {
    const std::size_t marker = line.find("::");
    if (marker == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = line.substr(marker + 2);
    const std::size_t      comma = rest.find(',');
    if (comma == std::string_view::npos || comma == 0) {
        return false;
    }
    const std::string_view type = rest.substr(0, comma);
    return type.size() <= 8 &&
           std::all_of(type.begin(), type.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0; });
}

std::vector<Url> expandM3u(const Url& url, ISource& source) {
    const std::string text = codecs::readAllText(source);

    std::vector<Url> entries;
    for (const std::string& line : codecs::splitLines(text)) {
        // An HLS manifest is a very different thing wearing the same extension.
        // Cog hands those to FFmpeg rather than treating them as a track list;
        // returning nothing here lets the decoder layer claim the URL instead.
        if (line.starts_with("#EXT-X-MEDIA-SEQUENCE") ||
            line.starts_with("#EXT-X-STREAM-INF") || line.starts_with("#EXT-X-TARGETDURATION")) {
            return {};
        }
        if (line.starts_with('#')) {
            continue;  // #EXTINF and friends carry only display metadata
        }
        if (isGameMusicSidecarLine(line)) {
            // Not a playlist at all. Returning nothing leaves the rip beside it
            // as the entry, which is the one that actually plays.
            return {};
        }
        entries.push_back(codecs::resolveEntry(line, url));
    }
    return entries;
}

std::vector<Url> expandPls(const Url& url, ISource& source) {
    const std::string text = codecs::readAllText(source);

    // PLS is an INI file whose track entries are FileN=path. The numbering gives
    // the order, which need not match the order the lines appear in.
    std::vector<std::pair<long, Url>> numbered;

    for (const std::string& line : codecs::splitLines(text)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = lowercased(std::string_view{line}.substr(0, equals));
        if (!key.starts_with("file")) {
            continue;
        }

        const std::string_view index{key.data() + 4, key.size() - 4};
        if (index.empty() ||
            !std::all_of(index.begin(), index.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }

        const std::string value = line.substr(equals + 1);
        if (value.empty()) {
            continue;
        }
        numbered.emplace_back(std::strtol(std::string{index}.c_str(), nullptr, 10),
                              codecs::resolveEntry(value, url));
    }

    std::stable_sort(numbered.begin(), numbered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<Url> entries;
    entries.reserve(numbered.size());
    for (auto& [number, entry] : numbered) {
        entries.push_back(std::move(entry));
    }
    return entries;
}

constexpr std::string_view kM3uExtensions[] = {"m3u", "m3u8"};
constexpr std::string_view kM3uMimeTypes[]  = {"audio/x-mpegurl", "audio/mpegurl"};
constexpr std::string_view kPlsExtensions[] = {"pls"};
constexpr std::string_view kPlsMimeTypes[]  = {"audio/x-scpls"};

}  // namespace
}  // namespace xpcog

void xpcog_register_playlists(xpcog::PluginRegistry& r) {
    r.addContainer({
        .name       = "M3uContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kM3uExtensions,
        .mimeTypes  = xpcog::kM3uMimeTypes,
        .expand     = &xpcog::expandM3u,
    });
    r.addContainer({
        .name       = "PlsContainer",
        .priority   = xpcog::kDefaultPriority,
        .extensions = xpcog::kPlsExtensions,
        .mimeTypes  = xpcog::kPlsMimeTypes,
        .expand     = &xpcog::expandPls,
    });
}
