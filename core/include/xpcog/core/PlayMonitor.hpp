// How much of the audible track has actually been played, and the two thresholds
// that depend on it.
//
// Port of the accumulator in Cog Audio/Chain/OutputNode.m
// (-incrementAmountPlayed:), which is where Cog fires both `reportPlayCount` at
// sixty seconds and `reportScrobble` at its own threshold. They are one
// mechanism there and one here, because they answer the same question: not
// "where is the playhead" but "how much of this has the listener heard".
//
// **The difference between those two is the whole point.** A track seeked to its
// last ten seconds has a playhead near the end and has not been listened to. Cog
// keeps them apart by accumulating *deltas* of the position and discarding any
// that is negative or larger than five seconds, on the theory that anything else
// is a seek rather than playback.
//
// XPCog does not need that filter, and the reason is worth writing down because
// it makes this port simpler than its original rather than more complicated:
// `AudioEngine::playedSeconds()` counts **frames delivered to the device**, not
// position within the track. It is monotonic by construction, it does not move
// while paused, and a seek does not move it at all -- there is nothing for a
// heuristic to clean up. So this accumulates the engine's clock directly and the
// five-second window has no counterpart here.
//
// What it still guards is the clock going *backwards*, which happens once: a new
// `play()` restarts the count at zero. That is treated as a re-base rather than
// as negative time, so a track beginning is never credited with the previous
// session's total.
//
// Nothing here reads a clock of its own. The caller supplies the engine's, which
// is what lets the whole class be tested by calling advance() with numbers.

#pragma once

#include <functional>
#include <utility>

namespace xpcog {

class PlayMonitor {
public:
    /// The two thresholds, both Cog's.
    struct Rules {
        /// Cog's `amountPlayedInterval >= 60.0`, after which a play is counted.
        double playCountSeconds = 60.0;

        /// Cog's `MIN(240.0, duration / 2.0)`: half the track, or four minutes,
        /// whichever comes first. Also Last.fm's documented rule, which is not a
        /// coincidence -- Cog's threshold exists to satisfy it.
        double scrobbleCapSeconds = 240.0;

        /// Cog's `if (duration >= 30.0)`, and Last.fm's: a track shorter than
        /// this is never scrobbled at all. Below it `scrobbleThreshold()` is 0,
        /// which means never rather than immediately.
        double scrobbleMinimumDuration = 30.0;
    };

    using Notify = std::function<void()>;

    /// Two constructors rather than one with `Rules rules = {}`, which is what
    /// this was and which only MSVC accepts.
    ///
    /// A default argument is part of the enclosing class's *declaration*, so it
    /// is parsed while `PlayMonitor` is still incomplete -- and `Rules` is a
    /// member of `PlayMonitor`, so its default member initializers are not
    /// usable yet. Clang says so plainly ("default member initializer for
    /// 'playCountSeconds' needed within definition of enclosing class"); GCC
    /// says it could not convert a brace-enclosed initializer list, which is the
    /// same complaint wearing a disguise. Both are right and MSVC is wrong, so
    /// this compiled on the machine it was written on and on none of the four
    /// CI jobs.
    ///
    /// Defaulting the constructor instead moves the initialiser to where the
    /// class is complete, which is the point at which `rules_` is actually
    /// built.
    PlayMonitor() = default;
    explicit PlayMonitor(Rules rules) : rules_(rules) {}

    /// Both fire at most once per track, on the caller's thread inside
    /// advance(). Neither is called for a track that never reaches its
    /// threshold.
    void onPlayCountReached(Notify notify) { playCountReached_ = std::move(notify); }
    void onScrobbleReached(Notify notify) { scrobbleReached_ = std::move(notify); }

    /// A new track is audible. `clockSeconds` is the engine's current total;
    /// `durationSeconds` is the track's length, or 0 when it has none -- a live
    /// stream, or a file whose decoder cannot say.
    ///
    /// A track with no duration gets no scrobble threshold, which is deliberate
    /// and matches Cog: half of an unknown length is not four minutes, and
    /// scrobbling a radio station's worth of audio as one track is worse than
    /// not scrobbling it.
    void beginTrack(double clockSeconds, double durationSeconds);

    /// Nothing is audible. Leaves the clock base alone, so a later beginTrack()
    /// re-bases from wherever the engine has got to.
    void clear();

    /// The engine's clock advanced. Fires whichever thresholds this crosses.
    void advance(double clockSeconds);

    /// Audible seconds of the current track. Zero when nothing is playing.
    [[nodiscard]] double playedSeconds() const noexcept { return played_; }

    /// The scrobble threshold for the current track, or 0 when it has none.
    [[nodiscard]] double scrobbleThreshold() const noexcept { return scrobbleAt_; }

    /// Whether each has already fired for this track.
    [[nodiscard]] bool playCountReported() const noexcept { return playCountDone_; }
    [[nodiscard]] bool scrobbleReported() const noexcept { return scrobbleDone_; }

private:
    Rules  rules_;
    Notify playCountReached_;
    Notify scrobbleReached_;

    bool   active_        = false;
    double lastClock_     = 0.0;
    double played_        = 0.0;
    double scrobbleAt_    = 0.0;
    bool   playCountDone_ = false;
    bool   scrobbleDone_  = false;
};

}  // namespace xpcog
