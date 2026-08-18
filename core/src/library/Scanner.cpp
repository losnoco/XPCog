#include "xpcog/core/library/Scanner.hpp"

#include "xpcog/core/NaturalOrder.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace xpcog {
namespace {

/// How deep a playlist may point at another playlist. Cog expands one level;
/// going further is useful (an .m3u of .cue files is a real thing) but has to
/// stop, or a playlist that names itself loops forever.
constexpr int kMaxContainerDepth = 8;

constexpr std::string_view kCueExtension = "cue";

[[nodiscard]] bool isPlaylistExtension(std::string_view extension) {
    return extension == "m3u" || extension == "m3u8" || extension == "pls" ||
           extension == "xspf";
}

/// Cog's ReplayGain tags arrive as strings: "-3.50 dB" for gains, "0.987654" for
/// peaks. strtof stops at the space, which is what makes the suffix harmless.
[[nodiscard]] std::optional<float> parseGain(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const std::string value{text};
    char*             end = nullptr;
    const float       gain = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) {
        return std::nullopt;
    }
    return gain;
}

}  // namespace

void promoteReplayGain(MetadataMap& tags, TrackProperties& properties) {
    ReplayGainInfo& gain = properties.replayGain;

    const auto take = [&tags](std::string_view key) -> std::optional<float> {
        const std::string_view text = tags.first(key);
        const auto             value = parseGain(text);
        if (value) {
            tags.remove(key);
        }
        return value;
    };

    gain.trackGain = take("replaygain_track_gain");
    gain.trackPeak = take("replaygain_track_peak");
    gain.albumGain = take("replaygain_album_gain");
    gain.albumPeak = take("replaygain_album_peak");

    if (const std::string_view soundcheck = tags.first("itunnorm"); !soundcheck.empty()) {
        gain.soundcheck = std::string{soundcheck};
        tags.remove("itunnorm");
    }

    // An embedded cue sheet describes the file's own track layout, so it belongs
    // with the stream properties rather than in the tag list a user sees.
    if (const std::string_view cuesheet = tags.first("cuesheet"); !cuesheet.empty()) {
        properties.cuesheet = std::string{cuesheet};
        tags.remove("cuesheet");
    }
}

Scanner::Scanner(const PluginRegistry& registry) : Scanner(registry, Options{}) {}

Scanner::Scanner(const PluginRegistry& registry, Options options)
    : registry_(registry), options_(options) {}

bool Scanner::isInteresting(const Url& url) const {
    const std::string extension = url.extension();
    if (extension.empty()) {
        return false;
    }
    if (extension == kCueExtension) {
        return options_.readCueSheets;
    }
    if (isPlaylistExtension(extension)) {
        return options_.readPlaylists;
    }

    const auto claimed = registry_.allExtensions();
    return std::binary_search(claimed.begin(), claimed.end(), extension) ||
           registry_.isContainer(url);
}

void Scanner::collectFiles(const Url& directory, std::vector<Url>& out) const {
    const auto root = directory.localPath();
    if (!root) {
        return;
    }

    std::vector<std::filesystem::path> found;
    std::error_code                    error;

    if (options_.recursive) {
        // skip_permission_denied rather than throwing: one unreadable folder in
        // a music library must not abort the whole scan.
        auto iterator = std::filesystem::recursive_directory_iterator{
            *root, std::filesystem::directory_options::skip_permission_denied, error};
        const auto end = std::filesystem::recursive_directory_iterator{};
        for (; !error && iterator != end; iterator.increment(error)) {
            if (cancelled()) {
                return;
            }
            if (iterator->is_regular_file(error)) {
                found.push_back(iterator->path());
            }
        }
    } else {
        auto iterator = std::filesystem::directory_iterator{
            *root, std::filesystem::directory_options::skip_permission_denied, error};
        const auto end = std::filesystem::directory_iterator{};
        for (; !error && iterator != end; iterator.increment(error)) {
            if (iterator->is_regular_file(error)) {
                found.push_back(iterator->path());
            }
        }
    }

    // Directory order is whatever the filesystem feels like. Sorting naturally
    // is what puts track 9 before track 10 in the playlist.
    std::sort(found.begin(), found.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return naturalLess(a.generic_string(), b.generic_string());
              });

    for (const auto& path : found) {
        Url url = Url::fromLocalPath(path);
        if (isInteresting(url)) {
            out.push_back(std::move(url));
        }
    }
}

void Scanner::expandInto(const Url& url, std::vector<Url>& out,
                         std::vector<std::string>& seen, int depth) const {
    if (cancelled()) {
        return;
    }

    const std::string key = url.toString();
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
        return;  // already added, or a playlist cycle
    }

    // A fragment means the URL already names one track *inside* a container --
    // "album.cue#2". Its extension is still "cue", so without this check the
    // track would be expanded back into the whole sheet, whose every track is
    // already in `seen`, and the scan would come back empty.
    const bool isTrackReference = !url.fragment().empty();

    // An extensionless HTTP URL may still be a playlist identified by its
    // Content-Type. isContainer() deliberately performs no I/O, so give those
    // URLs one expansion attempt; expandContainer() returns the URL unchanged
    // when the opened source is ordinary audio instead.
    const bool mayBeMimeContainer = url.extension().empty();
    if (isTrackReference ||
        (!registry_.isContainer(url) && !mayBeMimeContainer) ||
        depth >= kMaxContainerDepth) {
        seen.push_back(key);
        out.push_back(url);
        return;
    }

    // Mark the container itself as visited before recursing, so a playlist that
    // names itself terminates rather than recursing to the depth limit.
    seen.push_back(key);

    const std::vector<Url> tracks = registry_.expandContainer(url);
    if (tracks.size() == 1 && tracks.front() == url) {
        // Nothing claimed it after all -- expandContainer returns the input
        // unchanged in that case, and treating that as expansion would loop.
        out.push_back(url);
        return;
    }
    for (const Url& track : tracks) {
        expandInto(track, out, seen, depth + 1);
    }
}

std::vector<Url> Scanner::expand(std::span<const Url> inputs) const {
    std::vector<Url>         candidates;
    std::vector<std::string> seen;

    for (const Url& input : inputs) {
        if (cancelled()) {
            break;
        }
        const auto path = input.localPath();
        std::error_code error;
        if (path && std::filesystem::is_directory(*path, error)) {
            collectFiles(input, candidates);
        } else {
            candidates.push_back(input);
        }
    }

    std::vector<Url> expanded;
    expanded.reserve(candidates.size());
    seen.reserve(candidates.size());
    for (const Url& candidate : candidates) {
        expandInto(candidate, expanded, seen, 0);
    }
    return expanded;
}

bool Scanner::readMetadata(PlaylistEntry& entry) const {
    entry.error = false;
    entry.errorMessage.clear();

    const PluginCache::Stamp stamp =
        (cache_ != nullptr) ? PluginCache::stampFor(entry.url) : PluginCache::Stamp{};

    if (cache_ != nullptr) {
        if (auto cached = cache_->lookup(entry.url, stamp)) {
            // Everything except the identity, which belongs to the playlist row
            // rather than to the file.
            const TrackId id  = entry.id;
            const Url     url = entry.url;
            entry             = std::move(*cached);
            entry.id          = id;
            entry.url         = url;
            return !entry.error;
        }
    }

    // Tags first, then the decoder: a decoder's own metadata() carries the
    // things that change mid-stream (ICY titles, chained Ogg), and those should
    // win over the static tags rather than the other way round.
    MetadataMap tags = registry_.readMetadata(entry.url);

    const PluginRegistry::OpenResult opened = registry_.open(entry.url);
    if (!opened) {
        entry.error        = true;
        entry.errorMessage = "no decoder could open this file";
        entry.applyMetadata(tags);
        // Cached too: a folder of a thousand JPEGs should be attempted once,
        // and the stamp means fixing the file still invalidates the failure.
        if (cache_ != nullptr) {
            cache_->store(entry.url, stamp, entry);
        }
        return false;
    }

    entry.properties = opened.decoder->properties();
    tags.mergeFrom(opened.decoder->metadata());

    promoteReplayGain(tags, entry.properties);
    entry.applyMetadata(tags);

    if (cache_ != nullptr) {
        cache_->store(entry.url, stamp, entry);
    }
    return true;
}

std::vector<PlaylistEntry> Scanner::scan(std::span<const Url> inputs) const {
    const std::vector<Url> urls = expand(inputs);

    std::vector<PlaylistEntry> entries;
    entries.reserve(urls.size());

    for (std::size_t i = 0; i < urls.size(); ++i) {
        if (cancelled()) {
            break;
        }
        PlaylistEntry entry;
        entry.url = urls[i];
        static_cast<void>(readMetadata(entry));
        entries.push_back(std::move(entry));

        if (progress_) {
            progress_(i + 1, urls.size());
        }
    }
    return entries;
}

}  // namespace xpcog
