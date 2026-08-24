// The bridge between the toolkit-free engine and the widgets.
//
// This is where Cog's AppController + PlaybackController + the KVO web between
// them lands, and it is deliberately the *only* place that knows both worlds.
// Everything below it is toolkit-free; everything above it never touches
// AudioEngine, Playlist or the registry directly.
//
// Threading is the whole point of the class, and it runs in both directions.
//
// Inbound, AudioEngine::Delegate is called from the feeder thread -- never the
// interface's -- so each callback does nothing but hand a closure to the
// dispatcher. Touching a widget from there would be an intermittent crash rather
// than an obvious one, which is why the delegate methods here are three lines
// each and stay that way.
//
// Outbound, the two engine calls that block -- play() and stop() -- run on a
// SerialExecutor of this class's own, because starting a track opens its source
// and primes about a second and a half of audio. For a file that is
// microseconds. For a URL it is a network round trip, and a station that is slow
// to answer froze the window for as long as it took. Cog opens URLs from a
// background queue (-addURLsInBackground:) for exactly this reason.
//
// While a start is in flight the interface must not touch the engine at all,
// because the executor's thread is inside it reconfiguring the device. That is
// what `starting_` is for: the getters answer with neutral values and the cheap
// controls decline, rather than reading state that is being rebuilt.
//
// What changed from the Qt version is only how the hops are spelled. QThread plus
// a bare QObject that existed to own an event-loop slot became SerialExecutor;
// queued signals became an injected dispatcher and xpcog::Signal. Every atomic,
// every generation counter and every ordering comment below is unchanged, because
// none of them were about Qt.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/SerialExecutor.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <wx/event.h>
#include <wx/timer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpcog::app {

class PlaybackController : private AudioEngine::Delegate {
public:
    using Dispatcher = std::function<void(std::function<void()>)>;

    PlaybackController(const PluginRegistry& registry, Playlist& playlist,
                       Settings& settings, Dispatcher dispatch);
    ~PlaybackController() override;

    PlaybackController(const PlaybackController&)            = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    [[nodiscard]] Playlist& playlist() noexcept { return playlist_; }

    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool paused() const;

    /// Seconds into the audible track, and its total length. Both zero when
    /// nothing is playing.
    [[nodiscard]] double position() const;
    [[nodiscard]] double duration() const;

    /// Seconds of audio actually delivered to the device since playback began,
    /// across every track. Monotonic, does not move while paused, and unmoved by
    /// a seek -- which is what makes it the right clock for "how much of this
    /// has been listened to" as opposed to "where is the playhead".
    ///
    /// Forwarded from the engine rather than tracked here. PlayMonitor consumes
    /// it; see MainFrame.
    [[nodiscard]] double playedSeconds() const;

    [[nodiscard]] TrackId currentTrack() const;

    /// The rate the device negotiated, or zero when nothing is open. The spectrum
    /// needs it: its band table is built against Nyquist.
    [[nodiscard]] double sampleRate() const;

    /// The audio being played, for a visualiser to look at.
    ///
    /// Owned here because this owns the output that fills it, and the output
    /// writes to it from the device callback -- so its lifetime has to be at
    /// least the output's. Borrowed by whoever draws it.
    [[nodiscard]] AudioTap& tap() noexcept { return tap_; }

    // --- commands ----------------------------------------------------------

    /// Starts the playlist entry `id`, or resumes/starts the current one when
    /// `id` is absent.
    void playTrack(TrackId id);

    /// Starts `id` at `seconds` rather than at its top.
    ///
    /// For resuming where the listener left off, which playTrack() cannot do:
    /// it clears the resume position deliberately, because an ordinary gesture
    /// begins at the beginning. Seeking afterwards does not work either --
    /// seek() declines while a start is still in flight, which is exactly when
    /// a caller would be asking.
    ///
    /// `startPaused` opens the track and holds it, which is what Cog does when
    /// the last session was paused rather than playing
    /// (AppController.m:288, -playEntryAtIndex:startPaused:andSeekTo:).
    void resumeTrack(TrackId id, double seconds, bool startPaused);
    void playPause();
    void stop();
    void next();
    void previous();

    /// `seconds` into the audible track.
    void seek(double seconds);

    /// Moves playback to the device the settings now name.
    ///
    /// For a device or share-mode change, which the engine reads when it opens
    /// the device and therefore only when a track starts. Cog switches the
    /// device under the running stream, and so does this where it can:
    /// AudioEngine::switchOutputDevice() keeps the decoded audio and hands it to
    /// the new device, so the gap is the driver's rather than the track's.
    ///
    /// Where it cannot -- paused, or a device that will not run the format
    /// already queued -- the track is re-opened and seeks back to where it had
    /// reached. Does nothing when nothing is playing; the next track picks the
    /// new device up by itself.
    void reopenOutput();

    /// Asks the engine to re-read the DSP settings, so an equaliser change is
    /// heard on the track already playing rather than the next one.
    void reloadDsp();

    /// 0.0 to 1.0. Stored in settings so it survives a restart, as Cog does.
    void setVolume(double linear);
    [[nodiscard]] double volume() const;

    // --- notifications, all delivered on the interface's thread -------------

    /// The playing entry changed, or playback stopped (kInvalidTrackId).
    Signal<TrackId> currentTrackChanged;

    /// Published a few times a second while playing, for the seek bar and clock.
    Signal<double, double> positionChanged;

    Signal<bool, bool> playbackStateChanged;

    /// A playing stream renamed itself: the entry's tags have already been
    /// updated, and the row and the now-playing display need redrawing.
    Signal<TrackId> trackMetadataChanged;

    /// A start is in flight: the source is being opened, which for a URL is a
    /// network round trip. Nothing is audible yet, and the window is free --
    /// this exists so it can say so rather than simply looking stuck.
    Signal<TrackId> startPending;

    /// A file could not be opened. Playback carries on, matching Cog's
    /// behaviour of not stalling on one bad file.
    Signal<TrackId, std::string> playbackFailed;

private:
    // --- AudioEngine::Delegate, all called on the feeder thread ---------
    std::optional<Url> nextTrack() override;
    void               trackBegan(const Url& url) override;
    void               stoppedNaturally() override;
    void               trackFailed(const Url& url) override;
    void               outputSwitchFailed() override;
    void streamMetadataChanged(const Url& url, const MetadataMap& tags) override;

    void publishState();

    /// Queues a start on the executor. Returns immediately.
    void requestStart(TrackId id);
    /// The executor's answer, back on the interface's thread.
    void finishStart(TrackId id, bool opened, std::uint64_t generation);

    /// Re-opens the current track and returns to where it had reached. The
    /// fallback for an output change that could not be made under the running
    /// stream.
    void restartForOutputChange();

    const PluginRegistry& registry_;
    Playlist&             playlist_;
    Settings&             settings_;
    Dispatcher            dispatch_;

    // Declaration order is load-bearing: the engine borrows the ring and the
    // output, and the output borrows the ring, so the ring must outlive both and
    // be destroyed last.
    RingBuffer                    ring_;
    AudioTap                      tap_;
    std::unique_ptr<IAudioOutput> output_;
    std::unique_ptr<AudioEngine>  engine_;

    /// Drives positionChanged while playing. A wxTimer needs an event handler to
    /// deliver to and this class is not a window, so it owns a bare one.
    wxEvtHandler sink_;
    wxTimer      ticker_;

    /// Where play() and stop() actually run.
    ///
    /// Declared after the engine so it is destroyed first: a task still inside
    /// engine_->play() when the engine went away would be reading freed memory,
    /// and ~SerialExecutor joins.
    std::unique_ptr<SerialExecutor> starter_;

    /// Bumped by every start request. A result carrying a stale generation is
    /// dropped, so a burst of double-clicks settles on the last one rather than
    /// on whichever call happened to answer first.
    std::atomic<std::uint64_t> startGeneration_{0};

    /// Set between requesting a start and hearing back. Read from the
    /// interface's thread to decide whether the engine is safe to touch.
    std::atomic<bool> starting_{false};

    /// Entries that failed to open during the current gesture. The skip-to-next
    /// rule needs a memory across asynchronous hops for the same reason the
    /// engine's advance loop does: nextForPlayback() answers from the repeat
    /// rules, so with repeat on it offers the same dead entry for ever.
    std::vector<TrackId> failedStarts_;

    /// Where a start now in flight should land, or 0 to begin at the top.
    ///
    /// Carried through the start rather than applied after it, and that is the
    /// whole reason it exists: requestStart() is asynchronous and seek() refuses
    /// while a start is in flight, so seeking straight after playTrack() is a
    /// call that quietly does nothing. Restarting for a device change used to do
    /// exactly that, which sent the track back to its beginning.
    double resumeAt_ = 0.0;

    /// A resumed session that was paused when it ended. Held until the track
    /// is actually open, because playPause() declines while a start is in
    /// flight -- which is when resumeTrack() asks.
    bool pauseOnStart_ = false;

    /// What the engine is currently playing, as far as the interface knows.
    TrackId audible_ = kInvalidTrackId;
    bool    paused_  = false;
};

}  // namespace xpcog::app
