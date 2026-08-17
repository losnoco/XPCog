// Remembers what a scan found, so adding the same file twice reads it once.
//
// Cog has the same idea in Audio/PluginController.mm, keyed on the URL alone --
// which means retagging a file in another program and re-adding it brings back
// the *old* tags for the rest of the session. Keying on modification time and
// size as well fixes that: the entry is only reused while the file it came from
// has not changed.
//
// Deliberately in-memory and unbounded within a session. A scan of 50k files
// holds a few tens of megabytes of strings, and the playlist itself already
// holds the same data; a bounded cache would evict exactly the entries a bulk
// add is about to ask for again.

#pragma once

#include "xpcog/core/library/PlaylistEntry.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace xpcog {

class PluginCache {
public:
    /// What the file looked like when the entry was read. Two files with the
    /// same path but different stamps are different files as far as this cache
    /// is concerned.
    struct Stamp {
        std::int64_t modifiedSeconds = 0;
        std::int64_t sizeBytes       = 0;

        [[nodiscard]] friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    /// Reads the stamp from the filesystem. Zeroed for anything not local,
    /// which makes remote URLs uncacheable rather than wrongly cacheable.
    [[nodiscard]] static Stamp stampFor(const Url& url);

    /// The cached entry for `url`, if one was stored under the same stamp.
    /// The returned entry keeps its own id of zero; the caller owns identity.
    [[nodiscard]] std::optional<PlaylistEntry> lookup(const Url&   url,
                                                      const Stamp& stamp) const;

    void store(const Url& url, const Stamp& stamp, const PlaylistEntry& entry);

    void clear();

    [[nodiscard]] std::size_t size() const;

    /// Hits and misses since construction, for `xpcog-cli scan` and for asking
    /// whether the cache is doing anything at all.
    struct Statistics {
        std::uint64_t hits   = 0;
        std::uint64_t misses = 0;
    };
    [[nodiscard]] Statistics statistics() const;

private:
    struct Record {
        Stamp         stamp;
        PlaylistEntry entry;
    };

    // A scan may well be split across threads by the app layer, and a cache that
    // is only safe on one thread is a cache nobody can use from a worker.
    mutable std::mutex                             mutex_;
    std::unordered_map<std::string, Record>        records_;
    mutable Statistics                             statistics_;
};

}  // namespace xpcog
