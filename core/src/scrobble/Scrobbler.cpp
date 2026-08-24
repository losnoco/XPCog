#include "xpcog/core/scrobble/Scrobbler.hpp"

#include "xpcog/core/FilePath.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace xpcog {
namespace {

using namespace std::chrono_literals;

/// The first wait after a retryable failure, and the ceiling it doubles towards.
///
/// Thirty seconds rather than something smaller because the failure this exists
/// for is "there is no network", which does not resolve in a second; fifteen
/// minutes rather than something larger because the failure it *also* covers is
/// Last.fm being briefly unwell, and an hour's silence after a five-minute
/// outage is its own kind of broken.
constexpr auto kInitialBackoff = 30s;
constexpr auto kMaxBackoff     = 15min;

/// How long the worker sleeps when it has nothing to do at all. It is woken
/// explicitly whenever work arrives, so this only bounds how long a stopping_
/// flag can go unnoticed.
constexpr auto kIdlePoll = 5s;

[[nodiscard]] std::int64_t systemClock() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

}  // namespace

Scrobbler::Scrobbler(LastFmClient& client, std::filesystem::path queuePath,
                     std::function<std::int64_t()> clock)
    : client_(client),
      queuePath_(std::move(queuePath)),
      clock_(clock ? std::move(clock) : &systemClock) {
    loadQueue();
    worker_ = std::thread([this] { workerLoop(); });
}

Scrobbler::~Scrobbler() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wakeup_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Scrobbler::eligible(const ScrobbleTrack& track) {
    // Last.fm's own requirement. Refused here rather than submitted and rejected,
    // because a permanently ineligible entry in the queue is one the worker would
    // drop on its first attempt anyway -- and one that never reaches the queue
    // cannot be the reason a listener's history has a gap they cannot explain.
    return !track.artist.empty() && !track.title.empty();
}

void Scrobbler::setSession(Session session) {
    {
        std::lock_guard lock(mutex_);
        session_ = std::move(session);
        // A new session is a reason to try again now: the most likely way to get
        // here is the listener having just fixed the thing that was failing.
        backoff_ = 0ms;
    }
    wakeup_.notify_all();
}

Scrobbler::Session Scrobbler::session() const {
    std::lock_guard lock(mutex_);
    return session_;
}

void Scrobbler::setEnabled(bool enabled) {
    enabled_.store(enabled);
    if (enabled) {
        wake();
    }
}

bool Scrobbler::canSendLocked() const {
    return enabled_.load() && client_.configured() && session_.connected();
}

bool Scrobbler::active() const {
    if (!enabled_.load() || !client_.configured()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    return session_.connected();
}

void Scrobbler::onSessionInvalidated(std::function<void()> notify) {
    std::lock_guard lock(mutex_);
    sessionInvalidated_ = std::move(notify);
}

std::size_t Scrobbler::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void Scrobbler::wake() {
    {
        std::lock_guard lock(mutex_);
        backoff_ = 0ms;
    }
    wakeup_.notify_all();
}

void Scrobbler::nowPlaying(const ScrobbleTrack& track) {
    if (!active() || !eligible(track)) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        // Replaced rather than queued: only the most recent is worth sending,
        // and a backlog of "now playing" updates is a contradiction.
        nowPlaying_ = track;
    }
    wakeup_.notify_all();
}

void Scrobbler::submit(const ScrobbleTrack& track) {
    if (!active() || !eligible(track)) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(track);
        saveQueueLocked();
    }
    wakeup_.notify_all();
}

bool Scrobbler::drain(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idleChanged_.wait_for(lock, timeout, [this] {
        // Everything went; or the worker parked with a backoff and nothing more
        // will happen until it expires; or nothing can be sent at all, which is
        // the state a cleared session leaves behind and is just as final for
        // this call's purposes.
        return queue_.empty() || !canSendLocked() || (idle_ && backoff_ > 0ms);
    });
}

void Scrobbler::workerLoop() {
    for (;;) {
        std::optional<ScrobbleTrack> announce;
        std::vector<ScrobbleTrack>   batch;
        std::string                  sessionKey;

        {
            std::unique_lock lock(mutex_);

            idle_ = true;
            idleChanged_.notify_all();

            const auto wait = (backoff_ > 0ms) ? backoff_
                                               : std::chrono::milliseconds(kIdlePoll);
            // `canSendLocked()` is part of the predicate rather than a check
            // inside the body. Left out, the predicate is satisfied by a
            // non-empty queue alone, so a queued scrobble with no session wakes
            // the worker as fast as it can loop -- see canSendLocked().
            wakeup_.wait_for(lock, wait, [this] {
                if (stopping_) {
                    return true;
                }
                if (!canSendLocked()) {
                    return false;
                }
                return nowPlaying_.has_value() || (!queue_.empty() && backoff_ == 0ms);
            });

            if (stopping_) {
                return;
            }

            if (!canSendLocked()) {
                // Nothing can be sent. Drop any now-playing update rather than
                // letting it go stale in the slot, and park again -- the queue
                // is kept, because a session may yet arrive.
                nowPlaying_.reset();
                continue;
            }

            idle_ = false;

            // The wait expired, which is what clears a backoff: the next pass
            // through tries again.
            backoff_ = 0ms;

            sessionKey = session_.key;
            announce   = std::exchange(nowPlaying_, std::nullopt);

            // Prune entries that can never be accepted before choosing a batch,
            // so an aged-out scrobble at the head cannot wedge the queue.
            const std::int64_t now = clock_();
            const std::size_t  before = queue_.size();
            std::erase_if(queue_, [now](const ScrobbleTrack& track) {
                return track.startedAt > 0 && now - track.startedAt > kMaxAgeSeconds;
            });
            if (queue_.size() != before) {
                saveQueueLocked();
            }

            const std::size_t take = std::min(queue_.size(), LastFmClient::kMaxBatch);
            batch.assign(queue_.begin(),
                         queue_.begin() + static_cast<std::ptrdiff_t>(take));
        }

        // --- outside the lock: the network ------------------------------

        bool sessionDied = false;

        if (announce) {
            LastFmError error;
            if (client_.updateNowPlaying(*announce, sessionKey, &error) ) {
                // A success here is the cheapest evidence the network is back,
                // so it is worth retrying the queue immediately rather than
                // waiting out a backoff that is no longer true.
                std::lock_guard lock(mutex_);
                backoff_ = 0ms;
            } else if (error.kind == LastFmError::Kind::SessionInvalid) {
                sessionDied = true;
            }
        }

        bool        batchFailed    = false;
        bool        batchRetryable = false;
        std::size_t sent           = 0;

        if (!batch.empty() && !sessionDied) {
            LastFmError error;
            const auto  result = client_.scrobble(batch, sessionKey, &error);
            if (result) {
                sent = batch.size();
            } else if (error.kind == LastFmError::Kind::SessionInvalid) {
                sessionDied = true;
            } else {
                batchFailed    = true;
                batchRetryable = error.retryable();
            }
        }

        // --- back under the lock: record what happened ------------------

        std::function<void()> notify;
        {
            std::lock_guard lock(mutex_);

            if (sessionDied) {
                // Cleared before the callback runs, so a handler that asks is
                // told the truth.
                session_ = Session{};
                notify   = sessionInvalidated_;
            } else if (sent > 0) {
                const std::size_t drop = std::min(sent, queue_.size());
                queue_.erase(queue_.begin(),
                             queue_.begin() + static_cast<std::ptrdiff_t>(drop));
                saveQueueLocked();
                backoff_ = 0ms;
            } else if (batchFailed) {
                if (batchRetryable) {
                    backoff_ = (backoff_ == 0ms)
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                         kInitialBackoff)
                                   : std::min(backoff_ * 2,
                                              std::chrono::duration_cast<
                                                  std::chrono::milliseconds>(kMaxBackoff));
                } else {
                    // Permanently refused. Drop the batch rather than retry it:
                    // one poisoned entry must not hold up everything behind it.
                    const std::size_t drop = std::min(batch.size(), queue_.size());
                    queue_.erase(queue_.begin(),
                                 queue_.begin() + static_cast<std::ptrdiff_t>(drop));
                    saveQueueLocked();
                }
            }

            idleChanged_.notify_all();
        }

        // Outside the lock: the handler is the application's, and calling it
        // while holding this mutex would invite a lock order nobody declared.
        if (notify) {
            notify();
        }
    }
}

// --- persistence ---------------------------------------------------------

void Scrobbler::loadQueue() {
    std::error_code ec;
    if (!std::filesystem::exists(queuePath_, ec) || ec) {
        return;
    }

    std::ifstream file(queuePath_, std::ios::binary);
    if (!file) {
        return;
    }

    nlohmann::json root = nlohmann::json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        // A queue file we cannot read is left on disk rather than deleted: it is
        // the only copy of whatever it held, and overwriting it on the next
        // submission is soon enough.
        return;
    }

    std::lock_guard lock(mutex_);
    for (const auto& item : root) {
        if (!item.is_object()) {
            continue;
        }
        ScrobbleTrack track;
        track.title         = item.value("title", std::string{});
        track.artist        = item.value("artist", std::string{});
        track.album         = item.value("album", std::string{});
        track.albumArtist   = item.value("albumArtist", std::string{});
        track.musicBrainzId = item.value("mbid", std::string{});
        track.trackNumber   = item.value("trackNumber", 0);
        track.duration      = item.value("duration", 0.0);
        track.startedAt     = item.value("startedAt", std::int64_t{0});

        if (eligible(track) && track.startedAt > 0) {
            queue_.push_back(std::move(track));
        }
    }
}

void Scrobbler::saveQueueLocked() const {
    nlohmann::json root = nlohmann::json::array();
    for (const ScrobbleTrack& track : queue_) {
        nlohmann::json item;
        item["title"]     = track.title;
        item["artist"]    = track.artist;
        item["startedAt"] = track.startedAt;
        if (!track.album.empty()) {
            item["album"] = track.album;
        }
        if (!track.albumArtist.empty()) {
            item["albumArtist"] = track.albumArtist;
        }
        if (!track.musicBrainzId.empty()) {
            item["mbid"] = track.musicBrainzId;
        }
        if (track.trackNumber > 0) {
            item["trackNumber"] = track.trackNumber;
        }
        if (track.duration > 0.0) {
            item["duration"] = track.duration;
        }
        root.push_back(std::move(item));
    }

    std::error_code ec;
    std::filesystem::create_directories(queuePath_.parent_path(), ec);

    // Written beside and renamed over, so a crash mid-write cannot leave a
    // truncated queue where a valid one was. The queue is the only record of a
    // scrobble that has not been sent.
    std::filesystem::path temporary = queuePath_;
    temporary += ".tmp";

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return;
        }
        file << root.dump();
        if (!file) {
            return;
        }
    }

    std::filesystem::rename(temporary, queuePath_, ec);
    if (ec) {
        // Windows will not rename over an existing file on every filesystem;
        // remove and retry rather than lose the write.
        std::filesystem::remove(queuePath_, ec);
        std::filesystem::rename(temporary, queuePath_, ec);
    }
}

}  // namespace xpcog
