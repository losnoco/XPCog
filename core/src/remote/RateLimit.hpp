// Slowing down a peer that keeps guessing.
//
// The token is 64 hex characters, so guessing it is not a plan. But "not a plan"
// assumes the guesses cost something, and an unthrottled server on a LAN will
// answer 401 as fast as the network can carry the question. This makes the
// wrong answer cost a quarter of a second after the fifth one, which is the
// difference between a token and a token that can be tried at line rate.
//
// Bounded on purpose: a map keyed by peer address is a thing an attacker can
// grow, so it is capped and cleared wholesale when it fills. Losing the counts
// is the right failure -- it costs an attacker a fresh window and costs an
// ordinary user nothing, whereas an unbounded map is the denial of service the
// throttle was meant to prevent.

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xpcog::remote {

class RateLimit {
public:
    using Clock = std::chrono::steady_clock;

    /// Records a failed attempt from `peer` and answers how long to wait before
    /// replying. Zero until the allowance is spent.
    [[nodiscard]] std::chrono::milliseconds noteFailure(std::string_view peer) {
        const Clock::time_point now = Clock::now();

        std::lock_guard guard(mutex_);
        if (peers_.size() >= kMaxPeers) {
            peers_.clear();
        }

        Entry& entry = peers_[std::string{peer}];
        if (now - entry.windowStart > kWindow) {
            entry.windowStart = now;
            entry.failures    = 0;
        }
        ++entry.failures;

        return entry.failures > kAllowance ? kPenalty : std::chrono::milliseconds{0};
    }

    /// Forgets a peer, on the attempt that finally succeeded.
    void noteSuccess(std::string_view peer) {
        std::lock_guard guard(mutex_);
        peers_.erase(std::string{peer});
    }

private:
    static constexpr std::size_t              kMaxPeers  = 256;
    static constexpr std::size_t              kAllowance = 5;
    static constexpr std::chrono::seconds     kWindow{60};
    static constexpr std::chrono::milliseconds kPenalty{250};

    struct Entry {
        Clock::time_point windowStart = Clock::now();
        std::size_t       failures    = 0;
    };

    std::mutex                             mutex_;
    std::unordered_map<std::string, Entry> peers_;
};

}  // namespace xpcog::remote
