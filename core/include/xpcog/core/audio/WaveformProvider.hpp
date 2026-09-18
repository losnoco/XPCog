// Waveforms for the seek bar, produced off the interface thread.
//
// Two jobs at most: the track that is playing, and a guess at the one after it.
// The first is what the listener is looking at, so it fills in as it goes --
// the analyser's progress is dispatched back as it happens -- and anything
// else running is abandoned the moment it is asked for. The second runs only
// once the first is done, and is dropped unrun if a new request arrives, so a
// guess never delays the bar someone is watching.
//
// The cancel is a generation counter, the same shape as PlaybackController's
// start generation: every job carries the number it was posted under, the
// analyser checks it between reads, and a request or a cancel bumps it. There
// is no queue to purge -- a job that wakes up under the wrong number returns.
//
// Everything published goes through the dispatcher, and only after the job's
// generation has been checked again *on that thread*: a snapshot queued a
// moment before a cancel must not paint a waveform on a bar just told to be
// plain. And the closures dispatched hold the provider weakly. The application's
// dispatcher posts onto the process, which outlives the window that owns this,
// and a job mid-track has several snapshots in flight at any moment -- a
// closure that reached a destroyed owner would be a crash on quit.

#pragma once

#include "xpcog/core/Dispatcher.hpp"
#include "xpcog/core/SerialExecutor.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/audio/Waveform.hpp"
#include "xpcog/core/audio/WaveformCache.hpp"

#include <memory>

namespace xpcog {

class PluginRegistry;

class WaveformProvider {
public:
    WaveformProvider(const PluginRegistry& registry, WaveformCache cache, Dispatcher dispatch);

    /// Abandons whatever is running and joins it. Nothing is published after
    /// this returns.
    ~WaveformProvider();

    WaveformProvider(const WaveformProvider&)            = delete;
    WaveformProvider& operator=(const WaveformProvider&) = delete;

    /// The track now playing. Supersedes every job running or waiting,
    /// including a prefetch not yet started.
    void request(const Url& url);

    /// The track that will probably follow. Runs after the current request
    /// finishes; forgotten if another request arrives first.
    void prefetch(const Url& url);

    /// Abandons everything. Nothing further is published until the next
    /// request.
    void cancel();

    /// A summary for `url`, on the dispatcher's thread. Partial while the
    /// analysis runs (`analysed` says how far), complete at the end and for a
    /// cache hit. Never published for a track that cannot be summarised.
    [[nodiscard]] Signal<Url, std::shared_ptr<const WaveformSummary>>& updated() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    /// After impl_, so it is destroyed first: the running job holds impl_ and
    /// must have let go before impl_ can be released, and the join is what
    /// guarantees it has.
    SerialExecutor executor_;
};

}  // namespace xpcog
