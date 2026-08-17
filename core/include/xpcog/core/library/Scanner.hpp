// Turning what a user dropped on the window into playlist entries.
//
// Port of the useful half of Cog's PlaylistLoader.m: folder enumeration
// (-fileURLsAtPath:), container expansion, deduplication and metadata loading.
// The NSOperationQueue plumbing around it does not come along -- core stays
// single-threaded and synchronous, and the app decides what runs where. That is
// not a simplification for its own sake: Cog's loader interleaves progress
// reporting, Sentry spans and sandbox handles with the actual work, and none of
// those belong in a library.
//
// Qt-free, so the CLI and the tests scan without an event loop.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace xpcog {

class Scanner {
public:
    struct Options {
        bool recursive = true;
        /// Cog's `readCueSheetsInFolders` and `readPlaylistsInFolders`. A folder
        /// holding both a cue sheet and its FLAC would otherwise add every track
        /// twice.
        bool readCueSheets = true;
        bool readPlaylists = true;
    };

    // Not a defaulted argument: Options is still being defined at that point,
    // and its member initialisers are not usable until the class is complete.
    explicit Scanner(const PluginRegistry& registry);
    Scanner(const PluginRegistry& registry, Options options);

    /// Reuses metadata across repeated reads of the same unchanged file. The
    /// cache is borrowed, not owned, so its lifetime spans a whole session
    /// rather than one scan. Null means no caching.
    void setCache(PluginCache* cache) noexcept { cache_ = cache; }

    /// Called as each item is finished. Invoked on the calling thread.
    using ProgressFn = std::function<void(std::size_t done, std::size_t total)>;
    void setProgressCallback(ProgressFn callback) { progress_ = std::move(callback); }

    /// Asks an in-flight scan to stop. Safe to call from another thread; the
    /// scan returns what it has rather than failing.
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    void resume() noexcept { cancelled_.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    /// Expands folders, playlists and cue sheets into playable URLs, in a stable
    /// human order, with duplicates removed and the first occurrence kept.
    [[nodiscard]] std::vector<Url> expand(std::span<const Url> inputs) const;

    /// Fills in `entry` from its URL: stream properties from the decoder, tags
    /// from the metadata readers. Returns false and records the reason on
    /// `entry` when the file cannot be opened -- a broken file becomes a visible
    /// error row rather than disappearing.
    bool readMetadata(PlaylistEntry& entry) const;

    /// expand() followed by readMetadata() for each result.
    [[nodiscard]] std::vector<PlaylistEntry> scan(std::span<const Url> inputs) const;

private:
    /// Files in one directory tree, in natural order.
    void collectFiles(const Url& directory, std::vector<Url>& out) const;

    /// True when the scan should look inside this file at all.
    [[nodiscard]] bool isInteresting(const Url& url) const;

    void expandInto(const Url& url, std::vector<Url>& out,
                    std::vector<std::string>& seen, int depth) const;

    const PluginRegistry& registry_;
    Options               options_;
    PluginCache*          cache_ = nullptr;
    ProgressFn            progress_;
    mutable std::atomic<bool> cancelled_{false};
};

/// Promotes the ReplayGain and cue-sheet tags a reader returned into
/// `TrackProperties`, and removes them from the tag map.
///
/// Cog does this inside -setMetadata:, mixed in with the stream-property keys
/// (PlaylistEntry.m:633-676). Separated out here because it is the one piece of
/// that method still needed once properties are a struct -- and because it is
/// worth testing that "-3.50 dB" parses.
void promoteReplayGain(MetadataMap& tags, TrackProperties& properties);

}  // namespace xpcog
