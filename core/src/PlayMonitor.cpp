#include "xpcog/core/PlayMonitor.hpp"

#include <algorithm>

namespace xpcog {

void PlayMonitor::beginTrack(double clockSeconds, double durationSeconds) {
    begin(clockSeconds, durationSeconds, true);
}

void PlayMonitor::repeatTrack(double clockSeconds, double durationSeconds) {
    // A seam leaves the engine's clock running; a new play() restarts it. So a
    // clock that has not gone backwards is the track looping, and one that has
    // is the listener starting it again -- which counts, repeat-one or not.
    const bool looping = active_ && clockSeconds >= lastClock_;
    begin(clockSeconds, durationSeconds, !looping);
}

void PlayMonitor::begin(double clockSeconds, double durationSeconds,
                        bool freshListen) {
    active_       = true;
    lastClock_    = clockSeconds;
    played_       = 0.0;
    scrobbleDone_ = false;
    if (freshListen) {
        playCountDone_ = false;
    }

    // Cog's rule exactly (PlaybackController.m:900-905): below the minimum there
    // is no threshold at all, and 0 means never rather than immediately.
    if (durationSeconds >= rules_.scrobbleMinimumDuration) {
        scrobbleAt_ = std::min(rules_.scrobbleCapSeconds, durationSeconds / 2.0);
    } else {
        scrobbleAt_ = 0.0;
    }
}

void PlayMonitor::clear() {
    active_ = false;
    played_ = 0.0;
    // lastClock_ is deliberately kept: the engine's clock does not restart just
    // because nothing is audible, and the next beginTrack() re-bases anyway.
}

void PlayMonitor::advance(double clockSeconds) {
    if (!active_) {
        lastClock_ = clockSeconds;
        return;
    }

    const double delta = clockSeconds - lastClock_;
    lastClock_         = clockSeconds;

    // Backwards means the engine restarted its count -- a new play() rather than
    // negative time. Re-base and credit nothing, so the new track does not
    // inherit the previous session's total.
    if (delta < 0.0) {
        return;
    }
    played_ += delta;

    // Order matters only in that both can fire on one call, for a track short
    // enough that half of it is under a minute. Each fires at most once.
    if (!playCountDone_ && played_ >= rules_.playCountSeconds) {
        playCountDone_ = true;
        if (playCountReached_) {
            playCountReached_();
        }
    }
    if (!scrobbleDone_ && scrobbleAt_ > 0.0 && played_ >= scrobbleAt_) {
        scrobbleDone_ = true;
        if (scrobbleReached_) {
            scrobbleReached_();
        }
    }
}

}  // namespace xpcog
