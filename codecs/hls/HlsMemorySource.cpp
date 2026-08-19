#include "HlsMemorySource.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace xpcog::codecs {

HlsMemorySource::HlsMemorySource(Url url, std::string mimeType)
    : url_(std::move(url)), mimeType_(std::move(mimeType)) {}

HlsMemorySource::~HlsMemorySource() { HlsMemorySource::close(); }

bool HlsMemorySource::open(const Url& url) {
    url_ = url;
    return true;
}

bool HlsMemorySource::seek(std::int64_t /*offset*/, int /*whence*/) { return false; }

std::int64_t HlsMemorySource::tell() const {
    const std::lock_guard lock(mutex_);
    return position_;
}

const Url& HlsMemorySource::url() const { return url_; }

std::string HlsMemorySource::mimeType() const { return mimeType_; }

void HlsMemorySource::setIdentity(Url url, std::string mimeType) {
    url_      = std::move(url);
    mimeType_ = std::move(mimeType);
}

void HlsMemorySource::append(std::vector<std::byte> data) {
    if (data.empty()) {
        return;
    }
    const std::lock_guard lock(mutex_);
    if (closed_) {
        return;
    }
    chunks_.push_back(std::move(data));
    dataReady_.notify_all();
}

void HlsMemorySource::markEndOfStream() {
    const std::lock_guard lock(mutex_);
    eof_ = true;
    dataReady_.notify_all();
}

void HlsMemorySource::reset() {
    const std::lock_guard lock(mutex_);
    chunks_.clear();
    frontOffset_ = 0;
    position_    = 0;
    eof_         = false;
    spaceAvailable_.notify_all();
    dataReady_.notify_all();
}

std::size_t HlsMemorySource::bufferedSegments() const {
    const std::lock_guard lock(mutex_);
    return chunks_.size();
}

bool HlsMemorySource::waitForRoom(std::size_t limit, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    spaceAvailable_.wait_for(lock, timeout,
                             [this, limit] { return chunks_.size() < limit || closed_; });
    return chunks_.size() < limit && !closed_;
}

std::int64_t HlsMemorySource::read(void* buffer, std::int64_t bytes) {
    if (bytes <= 0) {
        return 0;
    }

    auto*        destination = static_cast<std::byte*>(buffer);
    std::int64_t total       = 0;

    std::unique_lock lock(mutex_);
    while (total < bytes) {
        if (closed_) {
            break;
        }

        if (chunks_.empty()) {
            // Only an empty-handed read waits. Cog's blocks until the caller's
            // full request can be satisfied, which at the tail of the queue
            // stalls a decoder that already had most of what it asked for --
            // a short read is legal and is what the bytes in hand support.
            if (total > 0 || eof_) {
                break;
            }
            // Otherwise there is nothing to return and a segment is a network
            // round trip away, so the decoder stops here until one lands. Woken
            // by append(), markEndOfStream(), reset() or close(); no polling.
            dataReady_.wait(lock, [this] { return !chunks_.empty() || eof_ || closed_; });
            if (chunks_.empty()) {
                break;  // end of stream, or shutting down
            }
        }

        const std::vector<std::byte>& front     = chunks_.front();
        const std::size_t             available = front.size() - frontOffset_;
        const auto                    take =
            std::min<std::size_t>(available, static_cast<std::size_t>(bytes - total));

        std::memcpy(destination + total, front.data() + frontOffset_, take);
        frontOffset_ += take;
        position_ += static_cast<std::int64_t>(take);
        total += static_cast<std::int64_t>(take);

        if (frontOffset_ >= front.size()) {
            chunks_.pop_front();
            frontOffset_ = 0;
            spaceAvailable_.notify_all();
        }
    }

    return total;
}

void HlsMemorySource::close() {
    const std::lock_guard lock(mutex_);
    closed_ = true;
    eof_    = true;
    chunks_.clear();
    frontOffset_ = 0;
    dataReady_.notify_all();
    spaceAvailable_.notify_all();
}

void HlsMemorySource::interrupt() { close(); }

}  // namespace xpcog::codecs
