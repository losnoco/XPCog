// Lyrics fetched on a worker, remembered, and handed back on the interface
// thread.
//
// LrclibClient is synchronous, as every client over IHttpClient is, and a
// lookup can take as long as the transport's timeout. The pane that wants the
// answer is redrawn on every selection change, so the request has to leave
// the interface thread and the answer has to come back to it -- which is the
// job here, in the shape ScanTask and the scrobbler already use: a
// SerialExecutor for the work, a Dispatcher for the hop back.
//
// Serial, and that is a feature rather than a limitation: arrowing down a
// playlist fires one lookup per track, and a pool would open a dozen
// connections to a free service on someone else's bandwidth for tracks the
// listener has already scrolled past. One at a time, in order, each cached on
// arrival, is what a well-behaved client looks like.
//
// **The cache is the other half of being well-behaved, and it is two-tier.**
// Every answer is kept in memory for the session and, through an ILyricsStore,
// on disk between sessions -- the application hands in the library's database.
// A track's words do not change, so a hit is kept for good; "not found" is
// kept for a week, because the service grows and a track nobody had
// transcribed last month may well be there now; a failure is kept for a
// minute and in memory only, so that offline the pane says so rather than
// queueing a timeout per keypress, and once back online it does not need a
// relaunch to notice. Selecting the same track twice in a session, or in the
// next one, never asks twice.
//
// In core rather than the app because nothing in it is about the toolkit: the
// same class could give `xpcog-cli` a `lyrics` command, and the tests drive it
// with FakeHttp, a dispatcher that is a queue, and a clock that is a number.

#pragma once

#include "xpcog/core/Dispatcher.hpp"
#include "xpcog/core/SerialExecutor.hpp"
#include "xpcog/core/lyrics/LrclibClient.hpp"
#include "xpcog/core/lyrics/LyricsStore.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xpcog {

class LyricsLookup {
public:
    enum class Outcome {
        Found,         ///< `lyrics` has the words.
        Instrumental,  ///< The service has the track and says it has no words.
        NotFound,      ///< The service does not have the track, or has no words for it.
        Failed,        ///< Could not ask, or the answer made no sense; see `error`.
    };

    struct Answer {
        Outcome     outcome = Outcome::NotFound;
        std::string lyrics;  ///< Plain, unsynced; empty unless Found.
        std::string synced;  ///< LRC, when the service had one; carried, not yet shown.
        LyricsError error;   ///< Filled in when Failed.
    };

    /// Called on the interface thread, through the dispatcher, with the answer.
    using Handler = std::function<void(const Answer&)>;

    /// Unix seconds. Injected so the tests can move time rather than wait.
    using Clock = std::function<std::int64_t()>;

    /// `http` and `store` are borrowed and must outlive the lookup; `store`
    /// may be null, in which case answers live for the session only.
    /// `dispatch` must stay callable for as long as the lookup lives, and need
    /// not run anything after: a completion that arrives once this is gone is
    /// dropped. A null `clock` means the system clock.
    LyricsLookup(IHttpClient& http, Dispatcher dispatch, ILyricsStore* store = nullptr,
                 std::string apiRoot = std::string{LrclibClient::kDefaultApiRoot},
                 Clock       clock   = nullptr);
    ~LyricsLookup();

    LyricsLookup(const LyricsLookup&)            = delete;
    LyricsLookup& operator=(const LyricsLookup&) = delete;

    /// Points the client at another server and forgets every answer held in
    /// memory, since they came from the old one. The store is left alone: it
    /// is the library's, and a listener trying a mirror for an afternoon
    /// should not lose a year of answers to it.
    void setApiRoot(const std::string& apiRoot);
    [[nodiscard]] std::string apiRoot() const { return client_.apiRoot(); }

    /// The remembered answer for `query`, from memory or the store, or nullopt
    /// when it has not been asked, is still being asked, or is old enough to
    /// ask again. Never Failed once its minute is up.
    [[nodiscard]] std::optional<Answer> cached(const LyricsQuery& query);

    /// Asks for `query`, unless the answer is remembered -- in which case
    /// `done` is dispatched at once with it -- or the same query is already in
    /// flight, in which case `done` is queued behind the earlier caller's and
    /// both hear the one answer. A query with no title or no artist is
    /// answered Failed without a request.
    void lookup(const LyricsQuery& query, Handler done);

    /// Whether `query` has been sent and not yet answered.
    [[nodiscard]] bool pending(const LyricsQuery& query) const;

    /// What two queries have to share to be the same question, and the key
    /// the store is written under.
    [[nodiscard]] static std::string keyOf(const LyricsQuery& query);

    /// How long each kind of answer is trusted, in seconds.
    static constexpr std::int64_t kFailureMemory  = 60;
    static constexpr std::int64_t kNotFoundMemory = 7 * 24 * 60 * 60;

    /// Answers held in memory before it is emptied wholesale. The store keeps
    /// them regardless; this only bounds a session's working set, and an
    /// eviction order would be machinery for a few kilobytes of text.
    static constexpr std::size_t kMaxInMemory = 512;

private:
    struct Remembered {
        Answer       answer;
        std::int64_t at = 0;  ///< Unix seconds when it arrived.
    };

    [[nodiscard]] std::int64_t now() const;

    /// Whether a remembered answer is still worth believing at `now`.
    [[nodiscard]] static bool fresh(const Remembered& entry, std::int64_t now);

    /// Runs on the interface thread: stores the answer and drains the handlers
    /// that were waiting for it.
    void deliver(const std::string& key, Answer answer);

    IHttpClient&  http_;
    LrclibClient  client_;
    Dispatcher    dispatch_;
    ILyricsStore* store_;
    Clock         clock_;

    mutable std::mutex                                    mutex_;
    std::unordered_map<std::string, Remembered>           memory_;
    std::unordered_map<std::string, std::vector<Handler>> waiting_;

    /// Held by every dispatched completion as a weak pointer, so one that
    /// runs after the destructor finds nobody home rather than a dead object.
    std::shared_ptr<int> alive_;

    /// Last, so the thread cannot start before the state it reads is built.
    SerialExecutor worker_;
};

}  // namespace xpcog
