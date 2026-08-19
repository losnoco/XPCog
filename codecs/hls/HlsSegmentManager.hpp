// The background fetcher. Port of Cog Plugins/HLS/HLSSegmentManager.m.
//
// Owns one thread that:
//
//   1. downloads the next segment through the plugin registry -- so a segment
//      is fetched by whatever source claims its scheme, exactly as any other
//      URL is -- and pushes the bytes into an HlsMemorySource;
//   2. parks while the memory source already holds `bufferSize` segments, which
//      is the back-pressure that stops a VOD playlist being downloaded whole;
//   3. re-fetches a live playlist every targetDuration/2 seconds and appends
//      whatever segments it has not seen before;
//   4. marks the memory source at end of stream once a finite playlist is
//      exhausted, so the decoder drains and stops rather than hanging.
//
// Every public method is safe to call from any thread.

#pragma once

#include "HlsMemorySource.hpp"
#include "HlsPlaylist.hpp"

#include "xpcog/core/Plugin.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace xpcog {
class PluginRegistry;
}

namespace xpcog::codecs {

class HlsSegmentManager {
public:
    HlsSegmentManager(HlsPlaylist playlist, const PluginRegistry& registry,
                      HlsMemorySource& memory);
    ~HlsSegmentManager();

    HlsSegmentManager(const HlsSegmentManager&)            = delete;
    HlsSegmentManager& operator=(const HlsSegmentManager&) = delete;

    /// Segments kept queued ahead of the decoder. Cog's default is five.
    void setBufferSize(std::size_t segments) { bufferSize_ = segments; }

    /// Downloads one segment synchronously. HlsDecoder uses this for the bytes
    /// the inner decoder needs before it can probe anything, and again on a seek
    /// -- both cases where failing has to be reported rather than retried.
    [[nodiscard]] std::optional<std::vector<std::byte>> download(std::size_t index);

    /// The Content-Type the most recent download reported, empty when the server
    /// sent none. Only a hint: see HlsDecoder, which sniffs the bytes first.
    [[nodiscard]] std::string lastMimeType() const;

    /// Starts the fetch thread at `index`, the next segment to download.
    void start(std::size_t index);

    /// Cancels in-flight downloads and asks the thread to exit, without waiting.
    /// Safe from a playback-control thread.
    void interrupt();

    /// Stops the fetch thread and joins it.
    void stop();

    [[nodiscard]] double totalDuration() const;
    [[nodiscard]] bool   isLive() const;

    /// A snapshot, since a live refresh appends to the list from another thread.
    [[nodiscard]] std::vector<HlsSegment> segments() const;

private:
    void run();
    /// Refetches and reparses a live playlist, appending unseen segments.
    void refreshLivePlaylist();
    /// Backs off after a failed fetch, and for a live stream eventually steps
    /// over the segment that keeps failing.
    void handleFailure();
    [[nodiscard]] bool stopping() const;
    /// Interruptible sleep, so a backoff does not delay teardown.
    void waitFor(std::chrono::milliseconds duration);

    const PluginRegistry* registry_ = nullptr;
    HlsMemorySource*      memory_   = nullptr;

    mutable std::mutex      mutex_;
    std::condition_variable wake_;

    HlsPlaylist            playlist_;
    std::set<std::int64_t> seenSequenceNumbers_;
    std::size_t            nextIndex_ = 0;
    bool                   stop_      = false;
    std::string            lastMimeType_;

    /// Sources of in-flight downloads, so interrupt() can unblock a read waiting
    /// on a server that will never answer. Shared rather than unique so the
    /// interrupting thread holds one alive across the call while the fetching
    /// thread is dropping it, and a list rather than one slot because a seek
    /// downloads its landing segment while the fetch thread is still working --
    /// with a single slot the second download would evict the first from the
    /// only record of it, and interrupt() would leave that one hanging.
    std::vector<std::shared_ptr<ISource>> activeSources_;

    std::size_t bufferSize_ = 5;
    std::thread thread_;

    /// Touched only by the fetch thread.
    int  consecutiveFailures_ = 0;
    std::chrono::steady_clock::time_point lastRefresh_{};
};

}  // namespace xpcog::codecs
