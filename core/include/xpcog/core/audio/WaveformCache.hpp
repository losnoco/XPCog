// Waveform summaries kept on disk, one file a track.
//
// Analysing a track means decoding it end to end, which is seconds of work for
// a result that is a couple of kilobytes and does not change until the file
// does. So it is kept, keyed the way PluginCache keys a scan: the URL together
// with the file's modification time and size, so a re-encoded file gets a new
// entry rather than the old one back. The URL keeps its fragment, so each cue
// track and each subsong is its own entry.
//
// The directory is injected, like the scrobble queue's path, because core does
// not know where the platform keeps caches. It is a cache in the platform's
// sense too: every entry can be remade from its track, so the operating system
// clearing the directory costs a second decode and nothing else. There is no
// pruning; two kilobytes for every track ever played is smaller than the
// bookkeeping to trim it.
//
// One known limit. For `album.cue#2` the stamp is the sheet's, because the
// audio behind it is the cue codec's business and core cannot see it. Replace
// the audio under an untouched sheet and the old waveform comes back until the
// sheet is touched. Not worth a seam through the plugin contract for.

#pragma once

#include "xpcog/core/audio/Waveform.hpp"
#include "xpcog/core/library/PluginCache.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace xpcog {

class Url;

class WaveformCache {
public:
    /// `directory` is created on the first store(), not here.
    explicit WaveformCache(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return directory_;
    }

    /// The summary stored for `url` as the file stands now, if any. Nothing for
    /// a URL that is not a local file, whose stamp is zero and matches nothing.
    [[nodiscard]] std::optional<WaveformSummary> load(const Url& url) const;

    /// Keeps `summary` for `url` as the file stands now. Does nothing for an
    /// incomplete summary or an uncacheable URL. Written beside and renamed
    /// over, so a crash mid-write leaves no half entry to be read back.
    /// Returns whether an entry was written.
    bool store(const Url& url, const WaveformSummary& summary) const;

    /// The file name an entry lives under: the first 32 hex characters of the
    /// SHA-256 of the URL and stamp, plus `.xpwf`.
    [[nodiscard]] static std::string keyFor(const Url& url, const PluginCache::Stamp& stamp);

private:
    std::filesystem::path directory_;
};

}  // namespace xpcog
