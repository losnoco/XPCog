// Scrobbling: what to submit, when, and what to do when it fails.
//
// Port of Cog Scrobbler/AudioScrobbler.swift, which is a faithful port up to the
// one line in it that says
//
//     // TODO: setup a local cache queue to send scrobbles in
//     // case the request fails
//
// That queue is the difference between a scrobbler and a scrobbler you can
// trust, so it is here. Cog submits on the playback thread and discards the
// result; a listener on a train loses the whole journey and is never told. What
// this does instead:
//
//   * Submissions go on a durable queue, written to disk as they are added, and
//     drained by a worker thread.
//   * A failure that could succeed later keeps the entry (see
//     `LastFmError::retryable`); one that could not drops it, because retrying a
//     rejected scrobble forever would block every later one behind it.
//   * The queue survives a restart, which is the case that motivates it: the
//     laptop was closed, not merely offline for a moment.
//
// **Now-playing updates are not queued**, deliberately. Last.fm keeps one for a
// few minutes and it never becomes part of the listening history, so a failed
// one has nothing to be retried *for*: by the time a retry succeeded it would be
// announcing a track that stopped playing long ago. It is the one call here that
// is genuinely fire-and-forget, which is also how Cog treats it.
//
// Threading. Everything public is safe to call from the interface's thread. The
// worker owns the network and the file; the queue is behind a mutex. The one
// callback that runs on the worker is `onSessionInvalidated`, and it says so.

#pragma once

#include "xpcog/core/scrobble/LastFmClient.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace xpcog {

class Scrobbler {
public:
    /// A granted Last.fm session. An empty `key` means "not connected", which is
    /// the state a fresh installation and a disconnected one share.
    struct Session {
        std::string key;
        std::string username;

        [[nodiscard]] bool connected() const noexcept { return !key.empty(); }
    };

    /// `client` is borrowed and must outlive the scrobbler.
    ///
    /// `queuePath` is where the pending queue is written. It is read at
    /// construction, so an unsent backlog from a previous run is picked up
    /// before anything new is added.
    ///
    /// `clock` supplies UTC seconds since the Unix epoch. Injected so the tests
    /// can hold time still: a queue whose retry policy depends on the wall clock
    /// is otherwise only testable by waiting.
    Scrobbler(LastFmClient& client, std::filesystem::path queuePath,
              std::function<std::int64_t()> clock = nullptr);

    ~Scrobbler();

    Scrobbler(const Scrobbler&)            = delete;
    Scrobbler& operator=(const Scrobbler&) = delete;

    /// Sets or clears the session. Clearing does not discard the queue: the
    /// scrobbles are still real, and a listener who reconnects should not have
    /// lost the ones that were waiting.
    void setSession(Session session);
    [[nodiscard]] Session session() const;

    /// The listener's preference. Off by default, as in Cog.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(); }

    /// Enabled, built with an API key, and holding a session. The one question
    /// the caller actually wants answered before submitting anything.
    [[nodiscard]] bool active() const;

    /// "This is playing now." Returns immediately; the request happens on the
    /// worker. Does nothing when inactive, or when the track lacks an artist or
    /// a title.
    void nowPlaying(const ScrobbleTrack& track);

    /// Queues a play for submission. `track.startedAt` must be set.
    ///
    /// Silently ignored when inactive or when the track lacks an artist or a
    /// title -- Last.fm would reject it, and queueing something that can only
    /// ever be rejected is how a queue stops draining.
    void submit(const ScrobbleTrack& track);

    /// Called on the **worker thread** when Last.fm reports the session key is
    /// no longer valid (error 9). The listener has to authorise again, so the
    /// caller should drop its stored credentials and say so; marshal to the
    /// interface's thread yourself.
    ///
    /// The scrobbler has already cleared its own copy by the time this runs.
    void onSessionInvalidated(std::function<void()> notify);

    /// How many plays are waiting. For the preferences pane, which is the only
    /// honest place to say "3 scrobbles waiting to be sent".
    [[nodiscard]] std::size_t pending() const;

    /// Wakes the worker to try the queue now, rather than at its next retry.
    /// Called when the network is likely to have come back -- a successful
    /// now-playing update is the cheap signal for that.
    void wake();

    /// Blocks until the queue is empty or nothing more can be sent. For tests
    /// and for shutdown; not something the interface should call.
    ///
    /// Returns false on timeout.
    bool drain(std::chrono::milliseconds timeout);

    /// Scrobbles older than this are dropped rather than submitted. Last.fm
    /// rejects timestamps far in the past, so an entry that has aged out can
    /// never succeed and would otherwise sit at the head of the queue.
    static constexpr std::int64_t kMaxAgeSeconds = 14 * 24 * 60 * 60;

private:
    void        workerLoop();
    void        loadQueue();
    void        saveQueueLocked() const;
    static bool eligible(const ScrobbleTrack& track);

    /// `active()` without taking the mutex, for callers that already hold it.
    ///
    /// This is load-bearing rather than a convenience. It is what the worker's
    /// wait predicate asks, and without it the predicate is true whenever the
    /// queue is non-empty -- including when nothing can be sent, which wakes the
    /// worker into a body that immediately gives up and waits again. That is a
    /// busy loop, and the state it happens in is a perfectly ordinary one: a
    /// queued scrobble and a session that was just invalidated.
    [[nodiscard]] bool canSendLocked() const;

    LastFmClient&                 client_;
    std::filesystem::path         queuePath_;
    std::function<std::int64_t()> clock_;

    mutable std::mutex      mutex_;
    std::condition_variable wakeup_;
    std::deque<ScrobbleTrack> queue_;

    /// The now-playing update waiting to be sent, if any. One slot rather than a
    /// queue: only the most recent is ever worth sending.
    std::optional<ScrobbleTrack> nowPlaying_;

    Session          session_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> stopping_{false};

    /// How long the worker waits before retrying after a retryable failure.
    /// Doubles up to a cap, and resets on any success.
    std::chrono::milliseconds backoff_{0};

    /// Raised while the worker is between wakeups with nothing left it can do.
    /// What drain() waits on.
    bool idle_ = true;
    mutable std::condition_variable idleChanged_;

    std::function<void()> sessionInvalidated_;

    std::thread worker_;
};

}  // namespace xpcog
