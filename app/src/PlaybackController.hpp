// The bridge between the Qt-free engine and the widgets.
//
// This is where Cog's AppController + PlaybackController + the KVO web between
// them lands, and it is deliberately the *only* place that knows both worlds.
// Everything below it is Qt-free; everything above it never touches
// AudioEngine, Playlist or the registry directly.
//
// Threading is the whole point of the class. AudioEngine::Delegate is called
// from the feeder thread -- never the GUI thread -- so each callback does
// nothing but emit a queued signal. Touching a widget from there would be an
// intermittent crash rather than an obvious one, which is why the delegate
// methods here are three lines each and stay that way.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/RingBuffer.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

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

    /// 0.0 to 1.0. Stored in settings so it survives a restart, as Cog does.
    void setVolume(double linear);
    [[nodiscard]] double volume() const;

signals:
    /// The playing entry changed, or playback stopped (kInvalidTrackId).
    void currentTrackChanged(TrackId id);

    /// Emitted a few times a second while playing, for the seek bar and clock.
    void positionChanged(double seconds, double duration);

    void playbackStateChanged(bool playing, bool paused);

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

    void emitState();

    const PluginRegistry& registry_;
    Playlist&             playlist_;
    Settings&             settings_;

    // Declaration order is load-bearing: the engine borrows the ring and the
    // output, and the output borrows the ring, so the ring must outlive both and
    // be destroyed last.
    RingBuffer                    ring_;
    std::unique_ptr<IAudioOutput> output_;
    std::unique_ptr<AudioEngine>  engine_;

    QTimer* ticker_ = nullptr;

    /// What the engine is currently playing, as far as the GUI thread knows.
    TrackId audible_ = kInvalidTrackId;
    bool    paused_  = false;
};

}  // namespace xpcog::app
