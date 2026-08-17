// The OS's idea of what is playing, and its transport controls.
//
// On macOS that is MediaPlayer.framework: MPNowPlayingInfoCenter fills in the
// Control Centre / Now Playing widget and the lock screen, and
// MPRemoteCommandCenter is how a Mac application receives the media keys at all
// -- the old approach of tapping the HID event stream stopped being viable, and
// Cog carries a whole CogRemoteControl subproject for what is now a dozen
// blocks. Registering the commands is what makes the keys arrive.
//
// The two are one class rather than two because on every platform they are one
// object: you cannot claim the keys without also saying what is playing, and a
// now-playing entry with no working controls is worse than none.
//
// Windows (SMTC) and Linux (MPRIS) are M5. Until then those platforms get the
// base class, which does nothing -- so the window wires up identically
// everywhere and gaining a platform is one subclass, not a new call site.

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

namespace xpcog::platform {

struct NowPlayingInfo {
    QString title;
    QString artist;
    QString album;
    /// Seconds. Zero means unknown, which is how a stream reports itself.
    double duration = 0.0;
    double position = 0.0;
    /// Null when the track has no art. The OS falls back to the app icon.
    QImage artwork;
};

class MediaIntegration : public QObject {
    Q_OBJECT

public:
    /// The implementation for this platform, or a do-nothing one where there is
    /// none yet. Never null, so callers have no branch to forget.
    [[nodiscard]] static MediaIntegration* create(QObject* parent = nullptr);

    explicit MediaIntegration(QObject* parent = nullptr) : QObject(parent) {}
    ~MediaIntegration() override = default;

    /// What is playing. Called on a track change, not per tick.
    virtual void setNowPlaying(const NowPlayingInfo& info) { (void)info; }

    /// Playing, paused or stopped, plus where the playhead is.
    ///
    /// Separate from setNowPlaying because the OS wants the elapsed time and
    /// the rate to move together: given both it extrapolates the position
    /// itself, so this is cheap to call on the transport's tick and the Now
    /// Playing widget still counts smoothly between calls.
    virtual void setPlaybackState(bool playing, bool paused, double position) {
        (void)playing;
        (void)paused;
        (void)position;
    }

    /// Nothing is playing. Removes the entry rather than leaving a stale one.
    virtual void clear() {}

signals:
    void playRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(double seconds);
};

}  // namespace xpcog::platform
