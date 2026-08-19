#include "HlsSegmentManager.hpp"

#include "../common/PlaylistText.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <algorithm>
#include <utility>

namespace xpcog::codecs {
namespace {

using namespace std::chrono_literals;

/// How long the fetch thread parks when the buffer is full or a live playlist
/// has announced nothing new. Short enough to notice a consumed segment or a
/// refresh deadline, long enough not to spin.
constexpr auto kIdleWait = 250ms;

/// Consecutive failures before a live stream steps over the offending segment.
/// A finite playlist never does: skipping there silently drops audio the user
/// asked for, and there is no broadcast clock to keep up with.
constexpr int kMaxConsecutiveFailures = 5;

/// Backoff after a failed fetch: 0.5s, 1s, 2s, 4s, then capped. Bounded so the
/// stream recovers promptly once the network does.
[[nodiscard]] std::chrono::milliseconds backoffFor(int failures) {
    const int shift = std::min(failures - 1, 3);
    return std::chrono::milliseconds{500} * (1 << shift);
}

/// The segment payload cap. A segment is a few seconds of audio; anything this
/// large is a misdirected request -- a login page, an error document, a whole
/// VOD file -- and reading it to the end would sit there filling memory.
constexpr std::size_t kMaxSegmentBytes = 128U * 1024U * 1024U;

}  // namespace

HlsSegmentManager::HlsSegmentManager(HlsPlaylist playlist, const PluginRegistry& registry,
                                     HlsMemorySource& memory)
    : registry_(&registry), memory_(&memory), playlist_(std::move(playlist)) {
    for (const HlsSegment& segment : playlist_.segments) {
        seenSequenceNumbers_.insert(segment.sequenceNumber);
    }
}

HlsSegmentManager::~HlsSegmentManager() { stop(); }

bool HlsSegmentManager::stopping() const {
    const std::lock_guard lock(mutex_);
    return stop_;
}

void HlsSegmentManager::waitFor(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, duration, [this] { return stop_; });
}

std::string HlsSegmentManager::lastMimeType() const {
    const std::lock_guard lock(mutex_);
    return lastMimeType_;
}

double HlsSegmentManager::totalDuration() const {
    const std::lock_guard lock(mutex_);
    return playlist_.totalDuration();
}

bool HlsSegmentManager::isLive() const {
    const std::lock_guard lock(mutex_);
    return playlist_.isLive;
}

std::vector<HlsSegment> HlsSegmentManager::segments() const {
    const std::lock_guard lock(mutex_);
    return playlist_.segments;
}

std::optional<std::vector<std::byte>> HlsSegmentManager::download(std::size_t index) {
    Url target;
    {
        const std::lock_guard lock(mutex_);
        if (stop_ || index >= playlist_.segments.size()) {
            return std::nullopt;
        }
        target = playlist_.segments[index].url;
    }
    if (target.empty()) {
        return std::nullopt;
    }

    // Through the registry, so a segment is fetched by whatever source claims
    // its scheme. Cog reaches for the AudioSource singleton; the effect is the
    // same and this one is injectable, which is what lets a test serve segments
    // without a socket.
    std::shared_ptr<ISource> source = registry_->makeSource(target);
    if (!source) {
        return std::nullopt;
    }

    {
        const std::lock_guard lock(mutex_);
        if (stop_) {
            return std::nullopt;
        }
        activeSources_.push_back(source);
    }

    // Dropped from the list on every exit path, but the local shared_ptr keeps
    // the source alive until this frame is done with it even if interrupt() is
    // running concurrently.
    const auto release = [this, &source] {
        const std::lock_guard lock(mutex_);
        const auto at = std::find(activeSources_.begin(), activeSources_.end(), source);
        if (at != activeSources_.end()) {
            activeSources_.erase(at);
        }
    };

    if (!source->open(target)) {
        release();
        return std::nullopt;
    }

    if (std::string mime = source->mimeType(); !mime.empty()) {
        const std::lock_guard lock(mutex_);
        lastMimeType_ = std::move(mime);
    }

    std::vector<std::byte> data;
    std::byte              buffer[16384];
    std::int64_t           got = 0;
    while ((got = source->read(buffer, sizeof(buffer))) > 0) {
        if (data.size() + static_cast<std::size_t>(got) > kMaxSegmentBytes) {
            source->close();
            release();
            return std::nullopt;
        }
        data.insert(data.end(), buffer, buffer + got);
    }
    source->close();
    release();

    // A read error and a cancellation both land here. Reporting failure either
    // way is right: the caller retries, and a cancelled manager will not.
    if (got < 0 || data.empty() || stopping()) {
        return std::nullopt;
    }
    return data;
}

void HlsSegmentManager::start(std::size_t index) {
    stop();

    {
        const std::lock_guard lock(mutex_);
        nextIndex_   = index;
        stop_        = false;
        lastRefresh_ = std::chrono::steady_clock::now();
    }
    consecutiveFailures_ = 0;
    thread_              = std::thread([this] { run(); });
}

void HlsSegmentManager::interrupt() {
    std::vector<std::shared_ptr<ISource>> active;
    {
        const std::lock_guard lock(mutex_);
        stop_  = true;
        active = activeSources_;
    }
    wake_.notify_all();

    // Outside the lock: interrupting a source can block briefly, and the fetch
    // thread needs the mutex to notice that it should stop.
    for (const std::shared_ptr<ISource>& source : active) {
        source->interrupt();
    }
}

void HlsSegmentManager::stop() {
    interrupt();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HlsSegmentManager::run() {
    while (!stopping()) {
        bool live           = false;
        int  targetDuration = 0;
        {
            const std::lock_guard lock(mutex_);
            live           = playlist_.isLive;
            targetDuration = playlist_.targetDuration;
        }

        if (live) {
            // RFC 8216 section 6.3.4: a client reloads a live playlist at least
            // once per half the target duration.
            const auto interval = std::max(
                std::chrono::milliseconds{targetDuration * 500},
                std::chrono::milliseconds{1000});
            if (std::chrono::steady_clock::now() - lastRefresh_ >= interval) {
                refreshLivePlaylist();
                if (stopping()) {
                    break;
                }
            }
        }

        if (!memory_->waitForRoom(bufferSize_, kIdleWait)) {
            continue;  // buffer full, closed, or the refresh deadline came first
        }

        std::size_t index = 0;
        bool        exhausted = false;
        {
            const std::lock_guard lock(mutex_);
            index     = nextIndex_;
            exhausted = index >= playlist_.segments.size();
            live      = playlist_.isLive;
        }

        if (exhausted) {
            if (!live) {
                // Nothing more will ever arrive. Cog breaks out of the loop
                // here too: the decoder needs the EOF to finish its last frames.
                memory_->markEndOfStream();
                break;
            }
            waitFor(kIdleWait);
            continue;
        }

        if (std::optional<std::vector<std::byte>> data = download(index)) {
            memory_->append(std::move(*data));
            const std::lock_guard lock(mutex_);
            nextIndex_ = index + 1;
            consecutiveFailures_ = 0;
        } else {
            if (stopping()) {
                break;
            }
            handleFailure();
        }
    }
}

void HlsSegmentManager::handleFailure() {
    ++consecutiveFailures_;
    waitFor(backoffFor(consecutiveFailures_));

    if (consecutiveFailures_ < kMaxConsecutiveFailures || !isLive()) {
        // A finite playlist keeps retrying. The memory source is deliberately
        // never marked at end of stream for a transient failure: a decoder that
        // has seen EOF cannot be revived, and there is still audio ahead.
        return;
    }

    const std::lock_guard lock(mutex_);
    ++nextIndex_;
    consecutiveFailures_ = 0;
}

void HlsSegmentManager::refreshLivePlaylist() {
    lastRefresh_ = std::chrono::steady_clock::now();

    Url playlistUrl;
    {
        const std::lock_guard lock(mutex_);
        if (stop_) {
            return;
        }
        playlistUrl = playlist_.url;
    }

    std::shared_ptr<ISource> source = registry_->makeSource(playlistUrl);
    if (!source) {
        return;
    }

    {
        const std::lock_guard lock(mutex_);
        if (stop_) {
            return;
        }
        activeSources_.push_back(source);
    }

    std::string text;
    if (source->open(playlistUrl)) {
        text = readAllText(*source);
    }
    source->close();
    {
        const std::lock_guard lock(mutex_);
        const auto at = std::find(activeSources_.begin(), activeSources_.end(), source);
        if (at != activeSources_.end()) {
            activeSources_.erase(at);
        }
        if (stop_) {
            return;
        }
    }

    const std::optional<HlsPlaylist> fresh = parseHlsPlaylist(text, playlistUrl);
    if (!fresh || fresh->isMaster) {
        return;
    }

    const std::lock_guard lock(mutex_);

    if (fresh->hasEndList) {
        // The publisher has just declared the stream finite. Adopting that lets
        // the loop drain what is left and stop instead of refreshing forever.
        playlist_.isLive     = false;
        playlist_.hasEndList = true;
    }

    for (const HlsSegment& segment : fresh->segments) {
        // By media sequence number, which is what identifies a segment across
        // refreshes -- the URL may repeat and the index certainly does, since a
        // live window slides.
        if (!seenSequenceNumbers_.insert(segment.sequenceNumber).second) {
            continue;
        }
        playlist_.segments.push_back(segment);
    }
}

}  // namespace xpcog::codecs
