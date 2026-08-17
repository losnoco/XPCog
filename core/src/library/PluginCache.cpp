#include "xpcog/core/library/PluginCache.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

namespace xpcog {

PluginCache::Stamp PluginCache::stampFor(const Url& url) {
    const auto path = url.localPath();
    if (!path) {
        return {};
    }

    std::error_code error;
    const auto      written = std::filesystem::last_write_time(*path, error);
    if (error) {
        return {};
    }
    const std::int64_t size =
        static_cast<std::int64_t>(std::filesystem::file_size(*path, error));
    if (error) {
        return {};
    }

    // file_time_type's epoch is unspecified. Only the difference matters here,
    // so the raw tick count is used rather than converting to a wall clock --
    // which is not portable before C++20's clock_cast and buys nothing.
    const auto ticks = written.time_since_epoch();
    return Stamp{
        std::chrono::duration_cast<std::chrono::seconds>(ticks).count(),
        size,
    };
}

std::optional<PlaylistEntry> PluginCache::lookup(const Url& url, const Stamp& stamp) const {
    // A zero stamp means "not a local file" or "could not be stat'ed". Treating
    // that as a cache key would make every remote URL collide with every other
    // unreadable one.
    if (stamp == Stamp{}) {
        return std::nullopt;
    }

    const std::lock_guard lock{mutex_};

    const auto it = records_.find(url.toString());
    if (it == records_.end() || !(it->second.stamp == stamp)) {
        ++statistics_.misses;
        return std::nullopt;
    }

    ++statistics_.hits;
    return it->second.entry;
}

void PluginCache::store(const Url& url, const Stamp& stamp, const PlaylistEntry& entry) {
    if (stamp == Stamp{}) {
        return;
    }

    PlaylistEntry copy = entry;
    // The id belongs to whichever playlist the entry was in, not to the file.
    // Handing it back out would give two playlist rows the same id.
    copy.id              = kInvalidTrackId;
    copy.shuffleIndex    = -1;
    copy.queuePosition   = -1;
    copy.currentPosition = 0.0;
    copy.stopAfter       = false;

    const std::lock_guard lock{mutex_};
    records_[url.toString()] = Record{stamp, std::move(copy)};
}

void PluginCache::clear() {
    const std::lock_guard lock{mutex_};
    records_.clear();
    statistics_ = {};
}

std::size_t PluginCache::size() const {
    const std::lock_guard lock{mutex_};
    return records_.size();
}

PluginCache::Statistics PluginCache::statistics() const {
    const std::lock_guard lock{mutex_};
    return statistics_;
}

}  // namespace xpcog
