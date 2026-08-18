// Port of Cog Plugins/HTTPSource/HTTPSource.m, on libcurl rather than
// NSURLSession -- so that sources stay Qt-free and identical on all three
// platforms, which QNetworkAccessManager would not be.
//
// The shape is Cog's: a worker thread owns the transfer and fills a ring, the
// decoder's thread drains it, and a seek that the ring cannot satisfy restarts
// the request with a Range header. The two pieces most likely to be wrong live
// next door with tests on them -- StreamBuffer for the ring and seek arithmetic,
// IcyDemux for the SHOUTcast framing. What is left here is the transport.
//
// Three notes on libcurl specifically:
//
//   * CURLOPT_HTTP09_ALLOWED is what makes SHOUTcast work at all. A server
//     answering "ICY 200 OK" is not speaking HTTP, and libcurl rejects the
//     response outright without it. Allowed, libcurl treats the reply as
//     HTTP/0.9 -- no headers, body from the first byte -- which is exactly the
//     shape IcyDemux's in-body header parser expects, and exactly what Cog's
//     handle_icy_headers() was written for.
//
//   * The stall timeout is checked in read(), not by CURLOPT_LOW_SPEED_TIME.
//     The obvious option is wrong here: when playback is paused the decoder
//     stops draining, the ring fills, the write callback blocks on purpose, and
//     libcurl counts that as a stalled transfer. It would reconnect every ten
//     seconds of pause. Cog checks the clock on the reading side for the same
//     reason, where "no data for ten seconds" can only mean the network.
//
//   * Cancellation is a generation counter rather than a handle poked from
//     another thread. Only the worker touches its CURL*, and both callbacks
//     compare the generation they were started with against the current one, so
//     a seek or a close ends the transfer at the next callback without anything
//     racing on libcurl state.

#include "IcyDemux.hpp"
#include "StreamBuffer.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Version.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace xpcog {
namespace {

using namespace std::chrono_literals;

/// Cog's HTTP_STREAMING_BUFFER_SIZE_DEFAULT. Cog makes it configurable through
/// the httpStreamingBufferSize default; XPCog does not, because a codec cannot
/// reach Settings -- core takes them by injection (see "Settings are generated
/// from one file" in docs/PORTING.md) and SourceDescriptor::create() takes no
/// arguments. Exposing it means threading Settings through PluginRegistry into
/// source construction, which is a registry change rather than a source one.
constexpr std::size_t kBufferSize = 0x40000;

/// Cog's TIMEOUT: seconds without a byte before the connection is assumed dead.
constexpr auto kStallTimeout = 10s;

/// Cog's redirectsRemaining.
constexpr long kMaxRedirects = 10;

/// How long a reconnect waits before trying again, and how many fruitless
/// attempts in a row end the stream. Cog reconnects immediately and forever,
/// which against a server that is simply down is a hot loop.
constexpr auto kRetryDelay    = 500ms;
constexpr int  kMaxFutileRetries = 5;

/// libcurl wants one global init before any handle exists. Never paired with
/// curl_global_cleanup: it is not safe to call while another thread might still
/// be inside libcurl, and a process that is exiting does not need it.
void ensureCurlInitialised() {
    static const bool once = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)once;
}

class HttpSource final : public ISource {
public:
    ~HttpSource() override { HttpSource::close(); }

    bool open(const Url& url) override {
        close();

        ensureCurlInitialised();

        url_       = url;
        requestUrl_ = url.withoutFragment().toString();
        stop_      = false;
        state_     = State::Connecting;
        seekToEnd_ = false;

        worker_ = std::thread([this] { run(); });

        // Wait for the headers, so mimeType() can pick a decoder. A stream that
        // never answers must not hang the caller for ever.
        std::unique_lock lock(mutex_);
        const bool ready = headersKnown_.wait_for(lock, 30s, [this] {
            return demux_.headersComplete() || state_ == State::Finished ||
                   state_ == State::Aborted;
        });

        if (!ready || state_ == State::Aborted) {
            lock.unlock();
            close();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool seekable() const override {
        const std::lock_guard lock(mutex_);
        return demux_.headers().contentLength > 0;
    }

    bool seek(std::int64_t offset, int whence) override {
        // A decoder measuring the file does seek(0, SEEK_END) then tell(). Cog
        // answers that without a request and refuses every other end-relative
        // seek, having no way to reach a position it cannot name absolutely.
        if (whence == SEEK_END) {
            if (offset != 0) {
                return false;
            }
            const std::lock_guard lock(mutex_);
            if (demux_.headers().contentLength <= 0) {
                return false;
            }
            seekToEnd_ = true;
            return true;
        }

        std::unique_lock lock(mutex_);
        seekToEnd_ = false;

        const std::int64_t target =
            (whence == SEEK_CUR) ? buffer_.tell() + offset : offset;
        if (target < 0) {
            return false;
        }

        if (buffer_.seekWithinWindow(target)) {
            spaceAvailable_.notify_all();
            return true;
        }

        // Out of the window: restart the transfer at the new offset.
        buffer_.reset(target);
        demux_.resetTransport();
        requestRestartLocked();
        lock.unlock();
        spaceAvailable_.notify_all();
        return true;
    }

    [[nodiscard]] std::int64_t tell() const override {
        const std::lock_guard lock(mutex_);
        if (seekToEnd_) {
            return demux_.headers().contentLength;
        }
        return buffer_.tell();
    }

    std::int64_t read(void* out, std::int64_t bytes) override {
        if (bytes <= 0) {
            return 0;
        }

        auto*            dst  = static_cast<std::byte*>(out);
        std::size_t      done = 0;
        const auto       want = static_cast<std::size_t>(bytes);
        std::unique_lock lock(mutex_);

        // Cog leaves seektoend set until the next seek, so tell() keeps
        // answering the file length after the decoder has read past it.
        seekToEnd_ = false;

        while (done < want) {
            buffer_.applyPendingSkip();

            if (const std::size_t got = buffer_.read(dst + done, want - done);
                got > 0) {
                done += got;
                spaceAvailable_.notify_all();
                continue;
            }

            if (stop_ || state_ == State::Aborted) {
                break;
            }
            if (state_ == State::Finished && buffer_.buffered() == 0) {
                // Covers a forward seek left pending past the end: there is
                // nothing further to skip into, so this is the end of input.
                break;
            }

            // Nothing buffered. Cog's timeout check sits exactly here: only a
            // reader that is waiting can tell the difference between a stalled
            // connection and a decoder that simply is not asking.
            if (state_ == State::Streaming &&
                std::chrono::steady_clock::now() - lastData_ > kStallTimeout) {
                requestRestartLocked();
                spaceAvailable_.notify_all();
                continue;
            }

            dataReady_.wait_for(lock, 100ms);
        }

        if (done == 0 && (stop_ || state_ == State::Aborted)) {
            return -1;
        }
        return static_cast<std::int64_t>(done);
    }

    void close() override {
        {
            const std::lock_guard lock(mutex_);
            if (!worker_.joinable() && !stop_) {
                return;
            }
            stop_ = true;
            ++generation_;
        }
        dataReady_.notify_all();
        spaceAvailable_.notify_all();
        headersKnown_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] const Url& url() const override { return url_; }

    [[nodiscard]] std::string mimeType() const override {
        const std::lock_guard lock(mutex_);
        return demux_.headers().contentType;
    }

    [[nodiscard]] MetadataMap takeUpdatedMetadata() override {
        const std::lock_guard lock(mutex_);
        return demux_.takeUpdatedMetadata();
    }

    void interrupt() override {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
            ++generation_;
        }
        dataReady_.notify_all();
        spaceAvailable_.notify_all();
        headersKnown_.notify_all();
    }

private:
    enum class State : std::uint8_t { Connecting, Streaming, Finished, Aborted };

    /// Ends the transfer in flight and has the worker start a new one from
    /// StreamBuffer::resumeOffset(). Caller holds the lock.
    void requestRestartLocked() {
        ++generation_;
        restartRequested_ = true;
        state_            = State::Connecting;
        lastData_         = std::chrono::steady_clock::now();
    }

    // --- worker thread ------------------------------------------------------

    void run() {
        int futileRetries = 0;

        for (;;) {
            std::uint64_t myGeneration = 0;
            std::int64_t  resumeFrom   = 0;
            {
                const std::lock_guard lock(mutex_);
                if (stop_) {
                    break;
                }
                restartRequested_ = false;
                myGeneration      = generation_;
                resumeFrom        = buffer_.resumeOffset();
                bytesThisRequest_ = 0;
                lastData_         = std::chrono::steady_clock::now();
            }

            const CURLcode rc = performRequest(myGeneration, resumeFrom);

            std::unique_lock lock(mutex_);
            if (stop_) {
                break;
            }
            if (restartRequested_ || generation_ != myGeneration) {
                continue;  // a seek or a stall asked for this; go round again
            }

            const bool gotBytes = bytesThisRequest_ > 0;
            // Only an icy-* header makes a clean close mean "reconnect". A plain
            // response without Content-Length -- chunked, say -- ends when it
            // ends, and treating a missing length as a live stream would
            // reconnect for ever at the end of every such file.
            const bool continuous = demux_.headers().continuous;
            const bool started    = gotBytes || demux_.headersComplete();

            if (rc == CURLE_OK && !continuous) {
                state_ = State::Finished;
                dataReady_.notify_all();
                headersKnown_.notify_all();
                break;
            }

            if (!started) {
                // Never got anywhere: the URL is wrong, or the host is not
                // there. Retrying would only repeat the same failure.
                state_ = State::Aborted;
                dataReady_.notify_all();
                headersKnown_.notify_all();
                break;
            }

            futileRetries = gotBytes ? 0 : futileRetries + 1;
            if (futileRetries >= kMaxFutileRetries) {
                state_ = State::Aborted;
                dataReady_.notify_all();
                break;
            }

            // Reconnect. The transport state resets -- the new response repeats
            // its headers and restarts the metaint cycle -- but the tags and the
            // buffer belong to the stream and survive.
            demux_.resetTransport();
            state_ = State::Connecting;
            stopping_.wait_for(lock, kRetryDelay, [this] { return stop_; });
        }

        const std::lock_guard lock(mutex_);
        if (state_ != State::Finished) {
            state_ = State::Aborted;
        }
        dataReady_.notify_all();
        headersKnown_.notify_all();
    }

    CURLcode performRequest(std::uint64_t myGeneration, std::int64_t resumeFrom) {
        std::unique_ptr<CURL, void (*)(CURL*)> handle(curl_easy_init(),
                                                      [](CURL* h) {
                                                          if (h != nullptr) {
                                                              curl_easy_cleanup(h);
                                                          }
                                                      });
        if (!handle) {
            return CURLE_FAILED_INIT;
        }

        Context context{this, myGeneration};

        curl_easy_setopt(handle.get(), CURLOPT_URL, requestUrl_.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, kMaxRedirects);
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(handle.get(), CURLOPT_HTTP09_ALLOWED, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, userAgent().c_str());

        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, &onHeader);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &context);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &onBody);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &onProgress);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &context);

        std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(
            curl_slist_append(nullptr, "Icy-MetaData: 1"), &curl_slist_free_all);
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());

        std::string range;
        if (resumeFrom > 0) {
            range = std::to_string(resumeFrom) + "-";
            curl_easy_setopt(handle.get(), CURLOPT_RANGE, range.c_str());
        }

        return curl_easy_perform(handle.get());
    }

    struct Context {
        HttpSource*   self;
        std::uint64_t generation;
    };

    [[nodiscard]] bool stale(std::uint64_t generation) const {
        const std::lock_guard lock(mutex_);
        return stop_ || generation != generation_;
    }

    static std::size_t onHeader(char* data, std::size_t size, std::size_t count,
                                void* user) {
        auto*             context = static_cast<Context*>(user);
        const std::size_t bytes   = size * count;
        auto*             self    = context->self;

        if (self->stale(context->generation)) {
            return 0;
        }

        {
            const std::lock_guard lock(self->mutex_);
            self->demux_.feedHeaderLine({data, bytes});
            self->lastData_ = std::chrono::steady_clock::now();
            if (self->demux_.headersComplete()) {
                if (self->state_ == State::Connecting) {
                    self->state_ = State::Streaming;
                }
            }
        }
        self->headersKnown_.notify_all();
        return bytes;
    }

    static std::size_t onBody(char* data, std::size_t size, std::size_t count,
                              void* user) {
        auto*             context = static_cast<Context*>(user);
        const std::size_t bytes   = size * count;
        auto*             self    = context->self;

        if (self->stale(context->generation)) {
            return 0;
        }

        const bool ok = self->demux_.feedBody(
            {reinterpret_cast<const std::byte*>(data), bytes},
            [self, context](const std::byte* audio, std::size_t length) {
                return self->store(audio, length, context->generation);
            });

        self->headersKnown_.notify_all();
        // Returning short is how libcurl is told to stop, which is what a
        // refused store means.
        return ok ? bytes : 0;
    }

    static int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t,
                          curl_off_t) {
        auto* context = static_cast<Context*>(user);
        return context->self->stale(context->generation) ? 1 : 0;
    }

    /// Moves audio into the ring, waiting while it is full. Returns short only
    /// when the transfer should end.
    std::size_t store(const std::byte* audio, std::size_t length,
                      std::uint64_t generation) {
        std::unique_lock lock(mutex_);
        std::size_t      done = 0;

        while (done < length) {
            if (stop_ || generation != generation_) {
                break;
            }

            if (const std::size_t put = buffer_.write(audio + done, length - done);
                put > 0) {
                done += put;
                bytesThisRequest_ += put;
                lastData_ = std::chrono::steady_clock::now();
                if (state_ == State::Connecting) {
                    state_ = State::Streaming;
                }
                dataReady_.notify_all();
                continue;
            }

            // Full. The decoder is behind, or paused; this is back-pressure
            // rather than a fault, so it waits rather than dropping audio.
            spaceAvailable_.wait_for(lock, 20ms);
        }
        return done;
    }

    [[nodiscard]] static const std::string& userAgent() {
        static const std::string agent =
            "XPCog/" + std::string{kVersionString};
        return agent;
    }

    mutable std::mutex      mutex_;
    std::condition_variable dataReady_;
    std::condition_variable spaceAvailable_;
    std::condition_variable headersKnown_;
    std::condition_variable stopping_;

    codecs::StreamBuffer buffer_{kBufferSize};
    codecs::IcyDemux demux_;

    Url         url_;
    std::string requestUrl_;
    std::thread worker_;

    State         state_            = State::Connecting;
    bool          stop_             = false;
    bool          restartRequested_ = false;
    bool          seekToEnd_        = false;
    std::uint64_t generation_       = 0;
    std::size_t   bytesThisRequest_ = 0;

    std::chrono::steady_clock::time_point lastData_ =
        std::chrono::steady_clock::now();
};

constexpr std::string_view kSchemes[] = {"http", "https"};

}  // namespace
}  // namespace xpcog

void xpcog_register_httpsource(xpcog::PluginRegistry& r) {
    r.addSource({
        .name     = "HttpSource",
        .priority = xpcog::kDefaultPriority,
        .schemes  = xpcog::kSchemes,
        .create   = []() -> xpcog::SourcePtr {
            return std::make_unique<xpcog::HttpSource>();
        },
        .available = nullptr,
    });
}
