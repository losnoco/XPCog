// Timestamped front-panel states, from whatever produced them to whatever draws
// them.
//
// Port of Cog Audio/Visualization/MIDIVisualizationController.m, and the same
// shape: one process-wide queue, keyed by track. That is a global, chosen
// deliberately over injection after the alternative was found not to exist --
// see below.
//
// ---------------------------------------------------------------------------
// Why this is not AudioTap
// ---------------------------------------------------------------------------
// The spectrum gets its synchronisation for free. AudioTap is filled *in the
// device callback*, so what is written is what is about to be heard and the
// latency compensation disappears rather than being reimplemented.
//
// A front panel cannot use that trick. Its state is produced while decoding,
// which runs ahead of the speaker by the whole buffer depth, and it does not
// travel in the samples -- there is nothing at the device callback to recover
// it from. So it has to be queued with a position and drained when that
// position becomes audible, which is what this is.
//
// ---------------------------------------------------------------------------
// Why a global
// ---------------------------------------------------------------------------
// Not because globals are fine, but because the obvious alternative is closed.
// The producer is a decoder, and a decoder belongs to the engine's feeder
// thread -- AudioEngine marshals even seek() across rather than touch it from
// the caller's thread, and it exposes no decoder at all. So "let the widget ask
// the decoder" is not a design that was rejected; it is one the UI thread
// cannot reach. Something shared and synchronised is required either way, and
// once that is true, injecting it buys only test isolation. Cog's shape was
// chosen so the two trees stay comparable.
//
// The cost is real and is paid in clear(): tests share a process, so a test
// that posts frames must clear them.
//
// ---------------------------------------------------------------------------
// Keyed by track, because two are alive at once
// ---------------------------------------------------------------------------
// AudioEngine asks for the next track "when the current track's decoder hits
// end of stream, while its audio is still playing out". So during a gapless
// seam two decoders are producing at the same time, both counting from zero in
// their own track. Frames therefore carry the URL they belong to, and a drain
// only ever returns the audible one's.

#pragma once

#include "xpcog/core/Url.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace xpcog {

/// One state of a device's front panel, positioned within its own track.
///
/// The state is opaque here on purpose: this holds bytes and a time, and only
/// the code that produced them knows how to draw them. That keeps the emulator
/// out of core.
struct PanelFrame {
    double                 seconds = 0.0;
    std::vector<std::byte> state;
};

class PanelFeed {
public:
    /// The one shared instance. See the header comment for why it is one.
    [[nodiscard]] static PanelFeed& instance();

    /// Whether anything has been recorded for the audible track.
    ///
    /// A blank display has two causes that look identical and mean opposite
    /// things: this track is not playing on a machine with a front panel at
    /// all -- an OPL3 has none -- or it is and nothing has happened yet.
    [[nodiscard]] bool producing() const;

    /// Appends a state for `track`. Called from the decode thread.
    void post(const Url& track, double seconds, std::span<const std::byte> state);

    /// Says which track the speaker has reached. States for anything else are
    /// held until it becomes the audible one, and discarded if it never does.
    void setAudibleTrack(const Url& track);

    /// What the panel looked like at `seconds` into the audible track.
    ///
    /// A *lookup into history*, not a queue drain, and that is the whole design
    /// -- it is how Cog does it, and the reason is what happens when a display
    /// is opened part-way through a track. Draining gives a display only what
    /// arrives after it opens, which is everything the decoder produced from
    /// wherever it had run ahead to; there is nothing at the position actually
    /// being heard, so the display shows one state and then sits frozen until
    /// the speaker catches up seconds later.
    ///
    /// Looking back has an answer immediately, and the right one. States are
    /// kept from the start of the track rather than from the moment something
    /// asked to see them.
    ///
    /// Returns the newest state at or before `seconds`. Before the first one --
    /// which is only the opening moment of a track -- the oldest is returned
    /// instead, so a display has something rather than nothing. Everything
    /// older than what is returned is dropped, since nothing can ask for it
    /// again without seeking, and a seek discards the lot.
    [[nodiscard]] std::optional<PanelFrame> stateAt(double seconds);

    /// Drops everything. For a seek, where every queued frame describes a
    /// moment that is no longer coming.
    void flush();

    /// Drops one track's frames, when it will not be played after all.
    void forget(const Url& track);

    /// Drops everything and forgets the audible track.
    ///
    /// For a stop, where nothing is audible any more and a display must go back
    /// to saying so. Stronger than flush() on purpose: the decoder is still
    /// winding down when the stop is asked for, so frames can arrive after the
    /// drop, and only forgetting the audible track makes those unreachable
    /// rather than a panel that reappears a moment after it was cleared.
    ///
    /// Also what keeps one test from seeing another's frames -- they share a
    /// process with each other, which is the cost of the singleton above.
    void clear();

private:
    PanelFeed() = default;

    struct Track {
        Url                  url;
        std::deque<PanelFrame> frames;
    };

    /// How much of a track's panel history is worth keeping. Cog trims to two
    /// minutes on a thirty-second timer; this trims on every post, which needs
    /// no timer and cannot fall behind. What is being bounded is a display that
    /// nobody is draining -- paused, or hidden after being shown.
    static constexpr double kMaxHistorySeconds = 120.0;
    /// And how many tracks. A gapless seam has two; anything past a handful
    /// means tracks are being queued and never played.
    static constexpr std::size_t kMaxTracks = 4;

    mutable std::mutex mutex_;
    std::deque<Track>  tracks_;
    Url                audible_;
    bool               haveAudible_ = false;
};

}  // namespace xpcog
