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
// All three platforms are implemented: MediaPlayer.framework on macOS, SMTC on
// Windows, MPRIS on Linux. Anything else gets the base class, which does nothing
// -- so the window wires up identically everywhere and gaining a platform is one
// subclass, not a new call site.
//
// The interface is the union of what the three can do, and the parts only one of
// them supports are the ones to be careful with. MPRIS is much the richest: it
// is a general remote-control protocol, so it asks to raise the window, to quit
// the application and to set the volume, none of which SMTC or macOS can
// express. Those arrive as signals like any other command, and the two
// implementations that cannot produce them simply never emit them.

#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>

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

    /// The output gain, 0..1.
    ///
    /// Only MPRIS uses this: it publishes the volume as a property a desktop
    /// environment both reads and writes, so the value has to be pushed here or
    /// the panel's slider sits at whatever it last set while the audio does
    /// something else. SMTC and macOS have no equivalent and ignore it.
    virtual void setVolume(float gain) { (void)gain; }

signals:
    void playRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(double seconds);

    /// Bring the window forward. MPRIS only -- it is how clicking a desktop
    /// panel's media widget is meant to reach the player.
    void raiseRequested();
    /// Quit the application. MPRIS only.
    void quitRequested();
    /// Set the output gain, 0..1. MPRIS only.
    void volumeRequested(float gain);
    /// Add and play this URL. MPRIS only -- it is the OpenUri method, which the
    /// protocol pairs with the list of schemes the player claims to support, so
    /// this exists rather than that list being a promise nothing keeps.
    void openUrlRequested(const QUrl& url);
};

}  // namespace xpcog::platform
