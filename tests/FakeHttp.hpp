// A transport that answers from a script instead of from a network.
//
// Shared by the Last.fm protocol tests and the scrobbler's queue tests, which
// need the same two things: to see exactly what was sent, and to decide exactly
// what comes back. Neither is possible against the real service, and a test that
// needed credentials and somebody's listening history would not be run.
//
// Thread-safe, because the scrobbler calls it from its worker.

#pragma once

#include "xpcog/core/net/HttpClient.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::test {

class FakeHttp final : public IHttpClient {
public:
    struct Call {
        std::string url;
        HttpParams  params;
        bool        post = false;
    };

    /// Queues a reply. Replies are handed out in the order they were added.
    void reply(long status, std::string body) {
        std::lock_guard lock(mutex_);
        replies_.push_back(HttpResponse{status, std::move(body), {}});
    }

    /// Queues a request that never reaches the server.
    void failTransport(std::string why) {
        std::lock_guard lock(mutex_);
        replies_.push_back(HttpResponse{0, {}, std::move(why)});
    }

    /// What every later call gets once the queue is empty. Without this a test
    /// whose worker retries more often than expected sees "no canned reply",
    /// which is a transport failure and therefore retryable -- an accidental
    /// infinite loop. Setting a default makes the steady state explicit.
    void setDefaultReply(long status, std::string body) {
        std::lock_guard lock(mutex_);
        fallback_ = HttpResponse{status, std::move(body), {}};
    }

    HttpResponse post(std::string_view url, const HttpParams& params) override {
        return record(url, params, true);
    }

    HttpResponse get(std::string_view url, const HttpParams& params) override {
        return record(url, params, false);
    }

    [[nodiscard]] std::size_t callCount() const {
        std::lock_guard lock(mutex_);
        return calls_.size();
    }

    [[nodiscard]] std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    /// The value sent for `key` in call `index`, or nullopt.
    [[nodiscard]] std::optional<std::string> sent(std::size_t      index,
                                                  std::string_view key) const {
        std::lock_guard lock(mutex_);
        if (index >= calls_.size()) {
            return std::nullopt;
        }
        for (const auto& [name, value] : calls_[index].params) {
            if (name == key) {
                return value;
            }
        }
        return std::nullopt;
    }

    /// How many calls named `method`.
    [[nodiscard]] std::size_t countOf(std::string_view method) const {
        std::lock_guard lock(mutex_);
        std::size_t     total = 0;
        for (const Call& call : calls_) {
            for (const auto& [name, value] : call.params) {
                if (name == "method" && value == method) {
                    total += 1;
                }
            }
        }
        return total;
    }

private:
    HttpResponse record(std::string_view url, const HttpParams& params, bool post) {
        std::lock_guard lock(mutex_);
        calls_.push_back(Call{std::string{url}, params, post});
        if (!replies_.empty()) {
            HttpResponse next = replies_.front();
            replies_.pop_front();
            return next;
        }
        if (fallback_) {
            return *fallback_;
        }
        return HttpResponse{0, {}, "no canned reply"};
    }

    mutable std::mutex          mutex_;
    std::vector<Call>           calls_;
    std::deque<HttpResponse>    replies_;
    std::optional<HttpResponse> fallback_;
};

}  // namespace xpcog::test
