// Playback transport and gapless handoff. Port of Cog Audio/AudioPlayer.m.
//
// Cog builds a BufferChain per track and opens the *next* chain while the current
// one is still playing out, so decoding never stalls at a track boundary
// (AudioPlayer.m -endOfInputReached:). XPCog keeps that shape: the feeder thread
// opens the next track the moment the current decoder reports end of stream, and
// keeps writing into the same ring. When consecutive tracks share a format there
// is no device reconfiguration and therefore no gap at all.
//
// Track-change notifications fire when the seam actually reaches the speaker, not
// when it is decoded -- Cog does this with -endOfInputPlayed after the output
// drains; here the output counts frames delivered and the engine compares that
// against recorded seam positions.

#pragma once

#include "xpcog/core/AudioFormat.hpp"
#include "xpcog/core/TrackProperties.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioConverter.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace xpcog {

class PluginRegistry;

enum class PlaybackStatus : std::uint8_t { Stopped, Playing, Paused };

class AudioEngine {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;

        /// Asked when the current track's decoder hits end of stream, while its
        /// audio is still playing out. Return the next URL to continue gaplessly,
        /// or nullopt to stop after the current track finishes.
        /// Mirrors Cog's -audioPlayer:willEndStream:.
        virtual std::optional<Url> nextTrack() { return std::nullopt; }

        /// The seam reached the speaker; `url` is now audible.
        virtual void trackBegan(const Url& /*url*/) {}

        /// Playback ended because there was nothing left to play.
        virtual void stoppedNaturally() {}

        /// A track could not be opened and was skipped.
        virtual void trackFailed(const Url& /*url*/) {}
    };

    /// `output` and `ring` are borrowed and must outlive the engine, and `output`
    /// MUST have been constructed around this same `ring` -- the engine writes
    /// into it and the output drains it. Taking both here rather than allocating
    /// a ring internally is deliberate: an engine-owned ring would silently be a
    /// different one from the output's, leaving each side waiting on the other.
    ///
    /// Passing an offline output makes the whole engine deterministic and
    /// device-free, which is how the gapless seam test runs in CI.
    /// `settings` is borrowed and read live, so a change to volumeScaling takes
    /// effect on the next track without restarting anything.
    AudioEngine(const PluginRegistry& registry, IAudioOutput& output, RingBuffer& ring,
                const Settings& settings);
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void setDelegate(Delegate* delegate) { delegate_ = delegate; }

    /// Opens `url` and starts playing. Replaces anything already playing.
    bool play(const Url& url);

    void stop();
    void pause();
    void resume();

    /// Jumps to `seconds` within the track that is currently audible.
    ///
    /// The request is handed to the feeder thread rather than performed here:
    /// the decoder belongs to that thread, and seeking it from the caller's
    /// would race with the decode in progress. Returns false when nothing is
    /// playing or the track cannot seek.
    bool seek(double seconds);

    /// Blocks until playback finishes on its own. For the CLI and tests.
    void waitUntilFinished();

    void setVolume(float gain);
    [[nodiscard]] float volume() const;

    [[nodiscard]] PlaybackStatus status() const noexcept {
        return status_.load(std::memory_order_relaxed);
    }

    /// Seconds of audio actually delivered to the device, across all tracks.
    [[nodiscard]] double playedSeconds() const;

    /// Position within the currently audible track.
    [[nodiscard]] double trackPositionSeconds() const;

    [[nodiscard]] std::uint64_t underrunCount() const { return output_.underrunCount(); }

private:
    struct Seam {
        /// Total frames delivered to the device before this track becomes audible.
        std::uint64_t framePosition = 0;
        Url           url;
    };

    void feederLoop();
    /// Applies a pending seek. Feeder thread only.
    void performSeek(std::int64_t frame);
    /// Opens `url`, returning false if nothing could decode it.
    bool openTrack(const Url& url);
    void closeTrack();
    /// Writes `chunk` into the ring, blocking until it all fits or we stop.
    bool writeToRing(const class AudioChunk& chunk);
    /// Writes already-converted samples, blocking until they all fit or we stop.
    bool writeSamples(const float* samples, std::size_t count);
    /// Applies the ReplayGain setting for the track now being decoded.
    void applyReplayGain(const TrackProperties& props);
    void publishSeams();

    const PluginRegistry& registry_;
    IAudioOutput&         output_;
    const Settings&       settings_;
    Delegate*             delegate_ = nullptr;

    /// Holds the device format fixed across tracks, which is what lets a track
    /// with a different sample rate join gaplessly.
    AudioConverter converter_;
    std::vector<float> converted_;

    RingBuffer& ring_;
    AudioFormat format_{};

    // The open track, touched only by the feeder thread.
    struct OpenTrack;
    std::unique_ptr<OpenTrack> track_;

    std::thread                 feeder_;
    std::atomic<bool>           running_{false};
    std::atomic<PlaybackStatus> status_{PlaybackStatus::Stopped};

    /// Total frames handed to the ring. Feeder-only.
    std::uint64_t framesWritten_ = 0;

    /// Frame the feeder should jump to, or -1 for none. Written by any thread,
    /// consumed by the feeder.
    std::atomic<std::int64_t> pendingSeek_{-1};

    /// Frames the device had played when the seek landed, and where in the track
    /// it landed. Together these re-base trackPositionSeconds(), which otherwise
    /// keeps counting from the start of the track and would report the old
    /// position after a jump.
    std::uint64_t seekPlayedBase_ = 0;
    std::uint64_t seekTrackBase_  = 0;

    mutable std::mutex seamMutex_;
    std::deque<Seam>   pendingSeams_;
    Url                audibleUrl_;
    std::uint64_t      audibleTrackStart_ = 0;

    mutable std::mutex      finishedMutex_;
    std::condition_variable finishedCv_;
    bool                    finished_ = false;
};

}  // namespace xpcog
