// The seam between the fetcher and the decoder: an ISource whose bytes arrive
// from somewhere else, a segment at a time.
//
// Port of Cog Plugins/HLS/HLSMemorySource.m. The producer is the segment
// manager's thread, which appends whole segment payloads; the consumer is the
// inner decoder, which reads and blocks when the queue runs dry.
//
// Deliberately not seekable. HLS seeking is not a byte offset -- it is "throw
// this away and start feeding me from a different segment" -- so HlsDecoder
// reaches for reset() and refills rather than for seek(), and reporting
// seekable() would invite the decoder underneath to try byte arithmetic across
// a segment boundary that means nothing.

#pragma once

#include "xpcog/core/Plugin.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace xpcog::codecs {

class HlsMemorySource final : public ISource {
public:
    HlsMemorySource(Url url, std::string mimeType);
    ~HlsMemorySource() override;

    // --- ISource ----------------------------------------------------------

    bool open(const Url& url) override;
    [[nodiscard]] bool seekable() const override { return false; }
    bool seek(std::int64_t offset, int whence) override;
    [[nodiscard]] std::int64_t tell() const override;
    std::int64_t read(void* buffer, std::int64_t bytes) override;
    void close() override;
    [[nodiscard]] const Url& url() const override;
    [[nodiscard]] std::string mimeType() const override;
    void interrupt() override;

    // --- producer side ----------------------------------------------------

    /// Queues a freshly fetched segment and wakes a waiting reader. Ignored once
    /// closed, so a fetch that lands during teardown cannot resurrect the queue.
    void append(std::vector<std::byte> data);

    /// No more data will arrive. A blocked reader drains what is left and then
    /// reports end of stream.
    ///
    /// One-way on purpose: a decoder that has seen EOF cannot be revived, which
    /// is why the fetcher never signals it for a transient network failure.
    void markEndOfStream();

    /// Drops everything queued and rewinds the reported position. Used by a
    /// seek, which discards the buffered future and refills from elsewhere.
    void reset();

    /// Segments fetched but not yet fully consumed. The fetcher's back-pressure
    /// signal.
    [[nodiscard]] std::size_t bufferedSegments() const;

    /// Waits until fewer than `limit` segments are queued, or `timeout` elapses,
    /// or the source closes. Returns true when there is room to fetch another.
    ///
    /// Cog's fetcher polls bufferedSegmentCount every 50 ms instead. Waiting is
    /// the same back-pressure without the idle wakeups, and the timeout is what
    /// keeps the fetcher able to notice a stop request or a playlist refresh
    /// deadline while a full buffer has it parked.
    bool waitForRoom(std::size_t limit, std::chrono::milliseconds timeout);

    /// The identity the inner decoder sees. Set once the first segment's format
    /// is known -- the extension and MIME type here are what select that decoder,
    /// so they cannot be decided before a byte has been fetched.
    void setIdentity(Url url, std::string mimeType);

private:
    mutable std::mutex      mutex_;
    std::condition_variable dataReady_;
    std::condition_variable spaceAvailable_;

    std::deque<std::vector<std::byte>> chunks_;
    std::size_t  frontOffset_ = 0;  ///< bytes of chunks_.front() already read
    std::int64_t position_    = 0;
    bool         eof_         = false;
    bool         closed_      = false;

    /// Not guarded: both are written by setIdentity() before the fetch thread
    /// exists and before any decoder has been handed this source, and only read
    /// afterwards.
    Url         url_;
    std::string mimeType_;
};

}  // namespace xpcog::codecs
