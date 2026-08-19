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

    /// True while something is actually displaying a panel.
    ///
    /// Producing costs real work -- for the SC-55 it is a comparison against
    /// the previous panel state on every emulated sample -- so nothing is
    /// produced unless someone is looking. A producer is expected to ask often
    /// enough that switching the display on takes effect promptly.
    [[nodiscard]] bool wanted() const noexcept;
    void               setWanted(bool wanted) noexcept;

    /// Appends a frame for `track`. Called from the decode thread.
    void post(const Url& track, double seconds, std::span<const std::byte> state);

    /// Says which track the speaker has reached. Frames for anything else are
    /// held until it becomes the audible one, and discarded if it never does.
    void setAudibleTrack(const Url& track);

    /// Removes and returns every frame of the audible track at or before
    /// `seconds`, oldest first.
    ///
    /// `seconds` is a position within the track, and the caller is expected to
    /// pass one derived from audio already delivered to the device --
    /// AudioEngine::trackPositionSeconds(). That is the one difference from Cog,
    /// which has no such clock to hand and instead takes the newest queued
    /// frame's timestamp and subtracts its estimate of the output latency. Both
    /// answer the same question; this one does not have to estimate.
    [[nodiscard]] std::vector<PanelFrame> take(double seconds);

    /// Whether anything has ever been posted since the display was switched on.
    ///
    /// For telling "nothing is producing" apart from "producing, nothing due
    /// yet" -- which from a blank panel look identical, and have completely
    /// different causes: the first means this track is not on a machine with a
    /// front panel at all.
    [[nodiscard]] bool producing() const noexcept;

    /// The oldest frame still queued for the audible track, without removing
    /// it. Empty when there is none.
    ///
    /// For the moment a display opens. The decoder runs far ahead of the
    /// speaker -- a synthesiser renders much faster than real time and the
    /// engine buffers deeply -- so when a panel is opened part-way through a
    /// track, everything queued is from a position that has not been reached
    /// yet, and a display that waits for one to become due sits blank for
    /// several seconds looking broken.
    ///
    /// So the first thing shown is the nearest state there is, which is ahead
    /// by however far the decoder has run. That is a real error and it is
    /// bounded, visible only until the speaker catches up, and then gone for
    /// good -- every frame after it is drained on time. Cog makes the opposite
    /// trade and never corrects: it draws the newest queued state minus its
    /// estimate of the device latency, which is ahead of the music for as long
    /// as the track plays.
    [[nodiscard]] std::optional<PanelFrame> peekEarliest() const;

    /// Drops everything. For a seek, where every queued frame describes a
    /// moment that is no longer coming.
    void flush();

    /// Drops one track's frames, when it will not be played after all.
    void forget(const Url& track);

    /// Drops everything and forgets the audible track. Tests share a process
    /// with each other; this is what keeps one from seeing another's frames.
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
    std::atomic<bool>  wanted_{false};
    std::atomic<bool>  produced_{false};
};

}  // namespace xpcog
