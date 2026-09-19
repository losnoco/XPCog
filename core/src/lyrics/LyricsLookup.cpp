#include "xpcog/core/lyrics/LyricsLookup.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace xpcog {
namespace {

/// A stored record read back as the answer it was. A record with no words
/// and no claim to be an instrumental is "nothing to show", whichever way the
/// service phrased it at the time.
[[nodiscard]] LyricsLookup::Answer answerOf(const StoredLyrics& record) {
    LyricsLookup::Answer answer;
    if (!record.known) {
        answer.outcome = LyricsLookup::Outcome::NotFound;
    } else if (record.instrumental) {
        answer.outcome = LyricsLookup::Outcome::Instrumental;
    } else if (!record.plain.empty()) {
        answer.outcome = LyricsLookup::Outcome::Found;
        answer.lyrics  = record.plain;
    } else {
        answer.outcome = LyricsLookup::Outcome::NotFound;
    }
    answer.synced = record.synced;
    return answer;
}

[[nodiscard]] StoredLyrics recordOf(const LyricsLookup::Answer& answer, std::int64_t at) {
    StoredLyrics record;
    record.known        = answer.outcome != LyricsLookup::Outcome::NotFound;
    record.instrumental = answer.outcome == LyricsLookup::Outcome::Instrumental;
    record.plain        = answer.lyrics;
    record.synced       = answer.synced;
    record.fetchedAt    = at;
    return record;
}

}  // namespace

LyricsLookup::LyricsLookup(IHttpClient& http, Dispatcher dispatch, ILyricsStore* store,
                           std::string apiRoot, Clock clock)
    : http_(http),
      client_(http, std::move(apiRoot)),
      dispatch_(std::move(dispatch)),
      store_(store),
      clock_(std::move(clock)),
      alive_(std::make_shared<int>(0)) {}

LyricsLookup::~LyricsLookup() {
    // Before the worker joins, so a completion it dispatches on the way out
    // finds the guard expired. The handlers still waiting are dropped with
    // it: whoever registered them is being torn down too.
    alive_.reset();
}

std::int64_t LyricsLookup::now() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string LyricsLookup::keyOf(const LyricsQuery& query) {
    // The same rounding the request uses, so two entries that differ by a
    // fraction of a second -- a cue sheet's arithmetic against the file's own
    // length -- are the one question they would be to the server.
    return query.artist + '\n' + query.title + '\n' + query.album + '\n' +
           std::to_string(std::llround(query.duration));
}

bool LyricsLookup::fresh(const Remembered& entry, std::int64_t now) {
    switch (entry.answer.outcome) {
    case Outcome::Failed:
        return now - entry.at < kFailureMemory;
    case Outcome::NotFound:
        return now - entry.at < kNotFoundMemory;
    case Outcome::Found:
    case Outcome::Instrumental:
        return true;
    }
    return false;
}

void LyricsLookup::setApiRoot(const std::string& apiRoot) {
    client_.setApiRoot(apiRoot);
    std::lock_guard lock(mutex_);
    memory_.clear();
}

std::optional<LyricsLookup::Answer> LyricsLookup::cached(const LyricsQuery& query) {
    const std::string  key = keyOf(query);
    const std::int64_t at  = now();

    {
        std::lock_guard lock(mutex_);
        if (const auto it = memory_.find(key); it != memory_.end()) {
            return fresh(it->second, at) ? std::optional{it->second.answer} : std::nullopt;
        }
    }

    if (store_ == nullptr) {
        return std::nullopt;
    }
    const auto stored = store_->load(key);
    if (!stored) {
        return std::nullopt;
    }
    Remembered entry{answerOf(*stored), stored->fetchedAt};
    if (!fresh(entry, at)) {
        return std::nullopt;
    }
    // Into memory, so the next selection of the same track is a map lookup
    // rather than a query -- and so a stale record read once is read once.
    std::lock_guard lock(mutex_);
    if (memory_.size() >= kMaxInMemory) {
        memory_.clear();
    }
    memory_[key] = entry;
    return entry.answer;
}

bool LyricsLookup::pending(const LyricsQuery& query) const {
    std::lock_guard lock(mutex_);
    return waiting_.contains(keyOf(query));
}

void LyricsLookup::lookup(const LyricsQuery& query, Handler done) {
    const std::string key = keyOf(query);

    if (const auto known = cached(query)) {
        // Through the dispatcher rather than called here, so a caller sees the
        // same shape whether the answer was remembered or fetched: never
        // re-entered from inside lookup().
        dispatch_([done = std::move(done), answer = *known] { done(answer); });
        return;
    }

    {
        std::lock_guard lock(mutex_);
        auto&           handlers = waiting_[key];
        handlers.push_back(std::move(done));
        if (handlers.size() > 1) {
            return;  // already in flight; the one answer reaches everybody
        }
    }

    std::weak_ptr<int> guard = alive_;
    worker_.post([this, guard, key, query] {
        LyricsError error;
        const auto  lyrics = client_.get(query, &error);

        Answer answer;
        if (lyrics) {
            answer.synced = lyrics->synced;
            if (lyrics->instrumental) {
                answer.outcome = Outcome::Instrumental;
            } else if (!lyrics->plain.empty()) {
                answer.outcome = Outcome::Found;
                answer.lyrics  = lyrics->plain;
            } else {
                // The service has the track but its entry has no words and
                // does not claim to be an instrumental: a placeholder, in
                // effect. To the listener that is "nothing to show".
                answer.outcome = Outcome::NotFound;
            }
        } else if (error.kind == LyricsError::Kind::NotFound) {
            answer.outcome = Outcome::NotFound;
        } else {
            answer.outcome = Outcome::Failed;
            answer.error   = error;
        }

        dispatch_([this, guard, key, answer = std::move(answer)]() mutable {
            if (guard.expired()) {
                return;
            }
            deliver(key, std::move(answer));
        });
    });
}

void LyricsLookup::deliver(const std::string& key, Answer answer) {
    const std::int64_t at = now();

    std::vector<Handler> handlers;
    {
        std::lock_guard lock(mutex_);
        if (memory_.size() >= kMaxInMemory) {
            memory_.clear();
        }
        memory_[key] = Remembered{answer, at};
        if (const auto it = waiting_.find(key); it != waiting_.end()) {
            handlers = std::move(it->second);
            waiting_.erase(it);
        }
    }

    // A failure says something about the network today, not about the track;
    // it stays in memory for its minute and goes no further. Everything else
    // is worth keeping across a relaunch -- a store that refuses is a store
    // problem, already reported through its own channel, and not this one's.
    if (store_ != nullptr && answer.outcome != Outcome::Failed) {
        static_cast<void>(store_->store(key, recordOf(answer, at)));
    }

    // Outside the lock: a handler is interface code and may well call back
    // into lookup() for the next track.
    for (const Handler& handler : handlers) {
        if (handler) {
            handler(answer);
        }
    }
}

}  // namespace xpcog
