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

    /// A start or a stop is in flight.
    ///
    /// The commands below silently decline while this is true -- which is right
    /// for a menu item, where the gesture is cheap to repeat and a queued
    /// second start would be worse than a dropped one. It is not right for a
    /// caller that has to *report* what happened: the REST remote control
    /// answers 409 on this rather than 200 for a command that did nothing.
    [[nodiscard]] bool busy() const;

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

    /// A file could not be opened, and the entry has been marked unplayable.
    ///
    /// A track that was *named* -- double-clicked, resumed -- stops here. Next
    /// and Previous keep looking, each in their own direction (see next() and
    /// previous()), and a track ending into a bad neighbour steps over no more
    /// than a few in a row -- AudioEngine's kMaxFailedAdvances.
    /// `id` is kInvalidTrackId when the URL that failed is not in the playlist
    /// any more.
    Signal<TrackId, std::string> playbackFailed;

    /// A sentence for the status area that belongs to no single entry: what a
    /// search for a playable track is doing, and how it ended.
    ///
    /// Separate from playbackFailed because that one names a row and marks it;
    /// this one is about the search itself, and publishing it through a signal
    /// that means "this entry is unplayable" would be a small lie the interface
    /// could not tell apart.
    Signal<std::string> statusNote;

private:
    /// Which way a search for a playable track is walking, or that none is.
    ///
    /// Next and Previous both search, and everything between the gesture and the
    /// track that finally plays has to know which button was pressed: the loud
    /// shape steps the current entry, the quiet one peeks, and both would walk
    /// away from the listener if they picked the direction for themselves.
    enum class Search { None, Forward, Backward };

    // --- AudioEngine::Delegate, all called on the feeder thread ---------
    std::optional<Url> nextTrack() override;
    void               trackBegan(const Url& url) override;
    void               stoppedNaturally() override;
    void               trackFailed(const Url& url) override;
    void               outputSwitchFailed() override;
    void streamMetadataChanged(const Url& url, const MetadataMap& tags) override;

    void publishState();

    /// Queues a start on the executor. Returns immediately.
    ///
    /// `hunt` is what Next and Previous ask for: should this one not open, take
    /// the entry beyond it *in that direction* and try again, rather than
    /// stopping. Every other caller leaves it at None, and passing it at all is
    /// what clears a hunt that was already running -- a gesture that starts
    /// something else has, by making it, said to stop looking.
    void requestStart(TrackId id, Search hunt = Search::None);
    /// The executor's answer, back on the interface's thread.
    void finishStart(TrackId id, bool opened, std::uint64_t generation);

    /// The silent search: opens candidates on the start thread and throws them
    /// away, so the track that is playing keeps playing until one of them turns
    /// out to be openable. `KeepPlayingWhileSkipping`.
    void startProbe(TrackId from, Search direction);
    void probeNext(TrackId from, std::uint64_t generation);
    void finishProbe(TrackId id, bool opens, std::uint64_t generation);

    /// Both searches end the same way, whether or not they found anything.
    void endSearch(bool found);
    /// Ends one without an answer, for a gesture that does not start anything
    /// and so would not otherwise supersede it.
    void cancelSearch();

    /// Re-opens the current track and returns to where it had reached. The
    /// fallback for an output change that could not be made under the running
    /// stream.
    void restartForOutputChange();

    /// What to call `id` in a sentence: the row's title, or `fallback` when the
    /// entry is not in the playlist any more.
    [[nodiscard]] std::string nameOf(TrackId id, const std::string& fallback) const;

    /// Marks `id` unplayable, or takes the mark off again once it plays. Both
    /// on the interface's thread, and both redraw the row.
    void markFailure(TrackId id);
    void clearFailure(TrackId id);

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

    /// Set between asking the engine to stop and it having done so.
    ///
    /// The mirror of starting_, and it exists for the mirrored reason: stop() is
    /// posted to the executor, so between the gesture and the engine acting on it
    /// the engine's own status still says Playing. playing() reads this instead,
    /// so a stop is reported the moment it is asked for rather than once the fade
    /// and the joins have finished.
    std::atomic<bool> stopping_{false};

    /// A Next or a Previous that is stepping past entries which will not open.
    ///
    /// `searching_` names the direction of the search now running -- and, for
    /// the loud shape, says that the start in flight belongs to it, so its
    /// failure should take the entry beyond rather than stop. `searched_` is
    /// what has already been tried, and it is what ends a search that has been
    /// all the way round a repeating playlist without finding anything.
    ///
    /// Neither needs to be cleared to cancel a search. Every start takes a new
    /// generation, and a superseded answer is dropped before it is read -- so a
    /// gesture that starts anything else has already ended the search by the
    /// time its own start comes back.
    Search               searching_ = Search::None;
    std::vector<TrackId> searched_;

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
