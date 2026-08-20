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
//
// Buffering is two-stage, and the reason is the DSP chain:
//
//   decoder -> converter -> preRing (deep) -> DSP chain -> ring (shallow) -> device
//
// The deep buffer is what absorbs a scheduling hiccup on the feeder thread
// without the device running dry, and it is ~3 seconds. Put the chain *behind*
// it -- the obvious arrangement, and what this engine did first -- and every DSP
// change is inaudible until those 3 seconds have played out: moving an equaliser
// slider appeared to do nothing for five seconds.
//
// So the depth goes ahead of the chain and only a shallow buffer follows it,
// which is why Cog gives each of its DSP nodes its own small ChunkList and its
// own thread. This is that arrangement with one thread for the whole chain
// instead of one per stage, since the stages are synchronous (see DSPNode).
//
// The chain therefore belongs to the DSP thread: it is the only one that calls
// process(), reads the settings into the coefficients, or resets the filters.
// That also means the chain runs in exactly one place, so no future path into
// the ring can forget to apply it.

#pragma once

#include "xpcog/core/AudioFormat.hpp"
#include "xpcog/core/MetadataMap.hpp"
#include "xpcog/core/TrackProperties.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioConverter.hpp"
#include "xpcog/core/audio/DSPNode.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/Fader.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

        /// A live output switch could not be made and playback stayed on the
        /// device it was already using -- the new one would not open, or would
        /// not run the format the queued audio is already in.
        ///
        /// The engine cannot do more than that on its own: it has no idea what
        /// is in the playlist, and re-opening the track is the caller's move.
        /// Restarting and seeking back is what happened unconditionally before
        /// switching under the stream existed, so the fallback is the old path
        /// rather than new code.
        virtual void outputSwitchFailed() {}

        /// The source reported tags that changed while the track was playing --
        /// a SHOUTcast StreamTitle naming the song currently on the radio.
        /// `url` is the track being decoded, which during a gapless handoff is
        /// already the next one; a live stream never hands off, so in practice
        /// it is the audible track.
        ///
        /// Called from the feeder thread, like every other delegate method.
        virtual void streamMetadataChanged(const Url& /*url*/,
                                           const MetadataMap& /*tags*/) {}
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

    /// Re-reads the DSP settings. Call after changing an equaliser setting.
    ///
    /// Push rather than poll: the chain's settings are 32 keys, and re-reading
    /// them per chunk to notice a slider move would cost more than the filter
    /// does. This only raises a flag, so it is safe to call from any thread --
    /// the DSP thread picks it up at the top of its next pass, which is the only
    /// place the coefficients may be rewritten without racing process().
    ///
    /// Unlike volumeScaling, this takes effect during the current track. An
    /// equaliser that waited for the next one would leave a user dragging a
    /// slider in silence, which reads as broken rather than as deferred.
    void reloadDsp() { dspDirty_.store(true, std::memory_order_relaxed); }

    /// Moves the running stream onto the device the settings now name, without
    /// restarting the track. For a device or share-mode change.
    ///
    /// Cog reconfigures its AUHAL under the running stream; this stops one
    /// device and starts the next, which is a gap of however long the driver
    /// takes to open rather than a gap of however long the track takes to
    /// re-open and seek. Nothing is lost across it: both rings keep what they
    /// hold, so the new device resumes at the very next sample.
    ///
    /// That only works while the format does not change, which is exactly the
    /// condition under which the queued audio is still meaningful -- it was
    /// converted for the format that was running. A device that will not run
    /// that format is reported through Delegate::outputSwitchFailed(), and
    /// playback stays where it was.
    ///
    /// Returns false when there is nothing playing to move, which includes
    /// being paused: the feeder services the switch, and a paused feeder is
    /// blocked on a ring nothing is draining. The caller's fallback covers it.
    /// A true return means the request was accepted, not that it succeeded --
    /// the work happens on the feeder thread, and failure arrives at the
    /// delegate.
    bool switchOutputDevice();

private:
    struct Seam {
        /// Total frames delivered to the device before this track becomes audible.
        std::uint64_t framePosition = 0;
        Url           url;
    };

    void feederLoop();
    /// Which device to open: the configured one if it is still there, matched
    /// by id and then by name, and otherwise the system default.
    [[nodiscard]] std::string chosenDeviceId() const;

    /// Applies a pending seek. Feeder thread only.
    void performSeek(std::int64_t frame);

    /// Moves the device under the running stream. Feeder thread only, for the
    /// same reason performSeek() is: this rewrites the position clock, and
    /// publishSeams() reads it on every pass.
    void performDeviceSwitch();

    /// Opens `config` and keeps it only if the device will run the format the
    /// queued audio is already in. Records what was asked for on success.
    [[nodiscard]] bool startDeviceForSwitch(const IAudioOutput::Config& config);

    /// Total frames delivered since play(), across device changes. Call with
    /// seamMutex_ held -- the two halves are only consistent together.
    [[nodiscard]] std::uint64_t totalFramesPlayedLocked() const;

    /// Asks the source whether the stream renamed itself, and tells the delegate.
    void pollStreamMetadata();

    /// Set by the decoder's change callback, which fires on the feeder thread
    /// from inside readAudio(); cleared by pollStreamMetadata() when it reports.
    /// Atomic because nothing in the contract says a decoder must notify from
    /// the thread that called it.
    std::atomic<bool> decoderTagsDirty_{false};

    /// Unblocks a read that may never return on its own, so stop() can join the
    /// feeder. A file's read() always completes; a live stream's parks until
    /// more audio arrives, which for a stalled stream is never.
    void interruptTrack();
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

    /// Pumps `preRing_` through the chain into `ring_`. Its own thread.
    void dspLoop();
    /// Reads the equaliser settings into the chain. DSP thread only.
    void applyDspSettings();

    /// The transport fade duration, or 0 when fading is switched off.
    [[nodiscard]] double fadeMilliseconds() const;

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

    /// Guards the *pointer*, not the track. Only the feeder swaps it, so the
    /// feeder reads it unlocked; stop() runs on another thread and must not
    /// read a half-swapped pointer while reaching in to interrupt.
    mutable std::mutex trackMutex_;

    std::thread                 feeder_;
    std::atomic<bool>           running_{false};
    std::atomic<PlaybackStatus> status_{PlaybackStatus::Stopped};

    /// The deep buffer, ahead of the chain. See the class comment.
    RingBuffer  preRing_;
    std::thread dsp_;

    /// True while the DSP thread holds a block that has left `preRing_` and not
    /// yet reached `ring_`.
    ///
    /// Without it, "has everything been played out" is answered by looking at the
    /// two rings -- and a block in flight between them is counted by neither. The
    /// end of a track was therefore declared while up to one block was still on
    /// its way to the device, which stop() then discarded. Small (a few
    /// milliseconds) and completely silent: it took comparing two renders of the
    /// same tone for it to show up at all, as one capture short by exactly one
    /// block.
    std::atomic<bool> dspBusy_{false};

    /// The chain, in order. Points at the members above rather than owning
    /// anything: the set of stages is fixed at compile time, so a vector of
    /// unique_ptr would buy indirection and nothing else.
    Equalizer             equalizer_;
    Fader                 fader_;
    std::vector<DSPNode*> chain_;
    std::atomic<bool>     dspDirty_{true};

    /// Set when pause() has started a fade out and the device still has to be
    /// stopped once it finishes. The feeder does that, because the fade has to be
    /// played to be heard and pause() must not block the caller for 200 ms.
    std::atomic<bool> pendingPause_{false};

    /// Bumped by the feeder when it discards the pre-seek audio, so the DSP
    /// thread knows to drop its own filter state and flush downstream. A counter
    /// rather than a flag because two seeks in quick succession must produce two
    /// resets, and a flag the DSP thread had not yet observed would collapse them.
    std::atomic<std::uint64_t> flushEpoch_{0};

    /// Total frames handed to the ring. Feeder-only.
    std::uint64_t framesWritten_ = 0;

    /// Frame the feeder should jump to, or -1 for none. Written by any thread,
    /// consumed by the feeder.
    /// The rate the *decoder* counts frames in, which is not always the rate the
    /// device runs at. They agreed for every PCM file, so the difference went
    /// unnoticed until DSD: 705,600 Hz of one-bit audio into a device running
    /// at 48,000, and a seek to a minute in asked the decoder for four seconds.
    /// Set wherever a decoder is installed -- openTrack() is the only place --
    /// and read from the caller's thread, hence atomic.
    std::atomic<double> trackRate_{0.0};

    std::atomic<std::int64_t> pendingSeek_{-1};

    /// Raised by switchOutputDevice(), consumed by the feeder. A flag rather
    /// than a queue: two device changes in flight mean the second one's answer
    /// is the one wanted, and both read the settings when they run.
    std::atomic<bool> pendingDeviceSwitch_{false};

    /// What was asked for when the running device was opened -- asked for, not
    /// granted, because that is what a later request has to be compared
    /// against. Feeder and play() only.
    std::string openDeviceId_;
    bool        openExclusive_ = false;

    /// Set when a switch has left the engine with no device at all: the new one
    /// would not open and neither would the one that was running. The feeder
    /// then ends the transport rather than going on writing into a ring nothing
    /// drains, which is a wedge rather than a stop.
    std::atomic<bool> deviceLost_{false};

    /// Frames the device had played when the seek landed, and where in the track
    /// it landed. Together these re-base trackPositionSeconds(), which otherwise
    /// keeps counting from the start of the track and would report the old
    /// position after a jump.
    std::uint64_t seekPlayedBase_ = 0;
    std::uint64_t seekTrackBase_  = 0;

    /// Set by performSeek(), consumed once the flush has been acknowledged.
    /// Feeder-thread only.
    std::uint64_t pendingSeekTrack_ = 0;
    bool          seekBasePending_  = false;

    /// Frames delivered by devices opened before the one now running.
    ///
    /// Every position the engine records -- seam positions, the seek base,
    /// framesWritten_ -- is one absolute count of frames since play(), and
    /// framesPlayed() is what they are measured against. A device restarts that
    /// counter at zero, and a live switch is the one thing that restarts it
    /// while a track is still playing. Carrying the difference here means the
    /// switch rewrites one number instead of every recorded position, and the
    /// rest of the engine never learns that the device changed.
    ///
    /// Under seamMutex_ for a reason worth stating: this and framesPlayed() are
    /// only meaningful added together, so a reader that took one before the
    /// switch and the other after would see the clock jump. The mutex is held
    /// across the whole device change for exactly that window.
    std::uint64_t      deviceFramesBase_ = 0;

    mutable std::mutex seamMutex_;
    std::deque<Seam>   pendingSeams_;
    Url                audibleUrl_;
    std::uint64_t      audibleTrackStart_ = 0;

    mutable std::mutex      finishedMutex_;
    std::condition_variable finishedCv_;
    bool                    finished_ = false;
};

}  // namespace xpcog
