// The bridge between the Qt-free engine and the widgets.
//
// This is where Cog's AppController + PlaybackController + the KVO web between
// them lands, and it is deliberately the *only* place that knows both worlds.
// Everything below it is Qt-free; everything above it never touches
// AudioEngine, Playlist or the registry directly.
//
// Threading is the whole point of the class, and it runs in both directions.
//
// Inbound, AudioEngine::Delegate is called from the feeder thread -- never the
// GUI thread -- so each callback does nothing but emit a queued signal.
// Touching a widget from there would be an intermittent crash rather than an
// obvious one, which is why the delegate methods here are three lines each and
// stay that way.
//
// Outbound, the two engine calls that block -- play() and stop() -- run on a
// starter thread of this class's own, because starting a track opens its source
// and primes about a second and a half of audio. For a file that is
// microseconds. For a URL it is a network round trip, and a station that is slow
// to answer froze the window for as long as it took. Cog opens URLs from a
// background queue (-addURLsInBackground:) for exactly this reason.
//
// While a start is in flight the GUI thread must not touch the engine at all,
// because the starter thread is inside it reconfiguring the device. That is what
// `starting_` is for: the getters answer with neutral values and the cheap
// controls decline, rather than reading state that is being rebuilt.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xpcog::app {

class PlaybackController : public QObject, private AudioEngine::Delegate {
    Q_OBJECT

public:
    PlaybackController(const PluginRegistry& registry, Playlist& playlist,
                       Settings& settings, QObject* parent = nullptr);
    ~PlaybackController() override;

    [[nodiscard]] Playlist& playlist() noexcept { return playlist_; }

    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool paused() const;

    /// Seconds into the audible track, and its total length. Both zero when
    /// nothing is playing.
    [[nodiscard]] double position() const;
    [[nodiscard]] double duration() const;

    [[nodiscard]] TrackId currentTrack() const;

    /// The rate the device negotiated, or zero when nothing is open. The spectrum
    /// needs it: its band table is built against Nyquist.
    [[nodiscard]] double sampleRate() const;

    /// The audio being played, for a visualiser to look at.
    ///
    /// Owned here because this owns the output that fills it, and the output writes
    /// to it from the device callback -- so its lifetime has to be at least the
    /// output's. Borrowed by whoever draws it.
    [[nodiscard]] AudioTap& tap() noexcept { return tap_; }

public slots:
    /// Starts the playlist entry `id`, or resumes/starts the current one when
    /// `id` is absent.
    void playTrack(TrackId id);
    void playPause();
    void stop();
    void next();
    void previous();

    /// `seconds` into the audible track.
    void seek(double seconds);

    /// Reopens the audio device, resuming where playback had reached.
    ///
    /// For a device or share-mode change, which the engine reads when it opens
    /// the device and therefore only when a track starts. Cog switches the
    /// device under the running stream; this restarts the track and seeks back,
    /// which is a short gap rather than a seam, and is honest about what it is
    /// doing. Does nothing when nothing is playing -- the next track will pick
    /// the new device up by itself.
    void reopenOutput();

    /// Asks the engine to re-read the DSP settings, so an equaliser change is
    /// heard on the track already playing rather than the next one.
    void reloadDsp();

    /// 0.0 to 1.0. Stored in settings so it survives a restart, as Cog does.
    void setVolume(double linear);
    [[nodiscard]] double volume() const;

signals:
    /// The playing entry changed, or playback stopped (kInvalidTrackId).
    void currentTrackChanged(TrackId id);

    /// Emitted a few times a second while playing, for the seek bar and clock.
    void positionChanged(double seconds, double duration);

    void playbackStateChanged(bool playing, bool paused);

    /// A playing stream renamed itself: the entry's tags have already been
    /// updated, and the row and the now-playing display need redrawing.
    void trackMetadataChanged(TrackId id);

    /// A start is in flight: the source is being opened, which for a URL is a
    /// network round trip. Nothing is audible yet, and the window is free --
    /// this exists so it can say so rather than simply looking stuck.
    void startPending(TrackId id);

    /// A file could not be opened. Playback carries on, matching Cog's
    /// behaviour of not stalling on one bad file.
    ///
    /// Named apart from AudioEngine::Delegate::trackFailed on purpose: a signal
    /// overloaded with an ordinary virtual in the same class is legal but
    /// confuses both moc and the reader.
    void playbackFailed(TrackId id, const QString& reason);

private:
    // --- AudioEngine::Delegate, all called on the feeder thread ---------
    std::optional<Url> nextTrack() override;
    void               trackBegan(const Url& url) override;
    void               stoppedNaturally() override;
    void               trackFailed(const Url& url) override;
    void streamMetadataChanged(const Url& url, const MetadataMap& tags) override;

    void emitState();

    /// Queues a start on the starter thread. Returns immediately.
    void requestStart(TrackId id);
    /// The starter thread's answer, back on the GUI thread.
    void finishStart(TrackId id, bool opened, std::uint64_t generation);

    const PluginRegistry& registry_;
    Playlist&             playlist_;
    Settings&             settings_;

    // Declaration order is load-bearing: the engine borrows the ring and the
    // output, and the output borrows the ring, so the ring must outlive both and
    // be destroyed last.
    RingBuffer                    ring_;
    AudioTap                      tap_;
    std::unique_ptr<IAudioOutput> output_;
    std::unique_ptr<AudioEngine>  engine_;

    QTimer* ticker_ = nullptr;

    /// Where play() and stop() actually run. `starter_` is a bare QObject whose
    /// only job is to own an event loop slot on that thread.
    QThread* starterThread_ = nullptr;
    QObject* starter_       = nullptr;

    /// Bumped by every start request. A result carrying a stale generation is
    /// dropped, so a burst of double-clicks settles on the last one rather than
    /// on whichever connection happened to answer first.
    std::atomic<std::uint64_t> startGeneration_{0};

    /// Set between requesting a start and hearing back. Read from the GUI thread
    /// to decide whether the engine is safe to touch.
    std::atomic<bool> starting_{false};

    /// Entries that failed to open during the current gesture. The skip-to-next
    /// rule needs a memory across asynchronous hops for the same reason the
    /// engine's advance loop does: nextForPlayback() answers from the repeat
    /// rules, so with repeat on it offers the same dead entry for ever.
    std::vector<TrackId> failedStarts_;

    /// What the engine is currently playing, as far as the GUI thread knows.
    TrackId audible_ = kInvalidTrackId;
    bool    paused_  = false;
};

}  // namespace xpcog::app
