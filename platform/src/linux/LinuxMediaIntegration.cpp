// MPRIS, the Linux desktop's media player protocol. The Linux counterpart of
// MacMediaIntegration.mm and WindowsMediaIntegration.cpp.
//
// MPRIS is not an API, it is a D-Bus contract: claim the well-known service name
// org.mpris.MediaPlayer2.<something>, export an object at
// /org/mpris/MediaPlayer2 implementing two interfaces, and every panel applet,
// lock screen, notification daemon and `playerctl` on the machine can drive
// playback. That is also why it is the richest of the three integrations -- it is
// a general remote control, where SMTC and MPNowPlayingInfoCenter are a card with
// buttons on it.
//
// Cog has no counterpart to port. This is the one media integration written from
// a specification rather than translated from Objective-C, which is worth saying
// because the usual check -- "does it do what Cog does" -- is unavailable. The
// reference is the MPRIS 2.2 spec; the observable behaviour is whatever
// `playerctl` and a desktop panel make of it.
//
// Four things the spec makes non-obvious, every one of them silent when wrong:
//
//   * Properties do not announce themselves. Qt exports Q_PROPERTY over D-Bus
//     happily, but nothing emits
//     org.freedesktop.DBus.Properties.PropertiesChanged on its own -- so a panel
//     reads the metadata once and then shows the first track for the rest of the
//     session. Every setter here signals explicitly.
//   * Every time is in microseconds, and signed. Seconds would be wrong by a
//     factor of a million, in the direction that looks like a track of no length.
//   * mpris:trackid is an object path, not a string. A string goes into the
//     variant map without complaint and is then discarded by the consumer, which
//     appears as metadata that never shows up rather than as an error.
//   * Position must NOT be announced through PropertiesChanged. It changes
//     continuously, so announcing it would be a bus signal per tick to every
//     client on the machine. Clients extrapolate from PlaybackStatus and Rate,
//     and Seeked() is how they are told their extrapolation just became wrong.
//
// Position and seeking: MPRIS Seek() is *relative* and SetPosition() is absolute,
// while the engine and the other two platforms deal only in absolute positions.
// The relative case is resolved here, against the position last pushed in
// setPlaybackState() -- which is why that value is kept rather than just
// forwarded on.
//
// This file is compiled only on Linux, so it is not built on the host the rest of
// the port is being written on; CI's Linux job is what compiles it. What CI
// cannot do is tell whether a panel likes the result, so see docs/PORTING.md.

#include "xpcog/platform/MediaIntegration.hpp"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace xpcog::platform {

namespace {
constexpr auto kObjectPath      = "/org/mpris/MediaPlayer2";
constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr auto kPropertiesIface = "org.freedesktop.DBus.Properties";

/// Seconds to the microseconds every MPRIS time is expressed in.
constexpr double kMicrosPerSecond = 1'000'000.0;
}  // namespace

class LinuxMediaIntegration;

// ---------------------------------------------------------------------------

/// org.mpris.MediaPlayer2 -- the application, not the playback.
class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")

    Q_PROPERTY(bool CanQuit READ canQuit CONSTANT)
    Q_PROPERTY(bool CanRaise READ canRaise CONSTANT)
    Q_PROPERTY(bool HasTrackList READ hasTrackList CONSTANT)
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes CONSTANT)

public:
    explicit MprisRoot(LinuxMediaIntegration* owner);

    [[nodiscard]] bool canQuit() const { return true; }
    [[nodiscard]] bool canRaise() const { return true; }

    /// No org.mpris.MediaPlayer2.TrackList. The playlist is not exported, and
    /// saying otherwise makes a client call methods that are not there.
    [[nodiscard]] bool hasTrackList() const { return false; }

    [[nodiscard]] QString identity() const { return QStringLiteral("XPCog"); }

    /// The basename of an installed .desktop file, which is how a panel finds the
    /// application's icon and localised name. Empty because XPCog installs none
    /// yet: naming a file that does not exist gets no icon either way, and adds a
    /// failed lookup.
    [[nodiscard]] QString desktopEntry() const { return {}; }

    [[nodiscard]] QStringList supportedUriSchemes() const {
        return {QStringLiteral("file")};
    }

    /// Deliberately coarse. An exact list would have to be derived from the codec
    /// registry and kept in step with it, and MPRIS consults this only to decide
    /// whether to offer OpenUri -- which is not implemented, so nothing reads it.
    [[nodiscard]] QStringList supportedMimeTypes() const {
        return {QStringLiteral("audio/*")};
    }

public Q_SLOTS:
    void Raise();
    void Quit();

private:
    LinuxMediaIntegration* owner_;
};

// ---------------------------------------------------------------------------

/// org.mpris.MediaPlayer2.Player -- the transport.
class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")

    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Rate READ rate CONSTANT)
    Q_PROPERTY(double MinimumRate READ minimumRate CONSTANT)
    Q_PROPERTY(double MaximumRate READ maximumRate CONSTANT)
    Q_PROPERTY(bool CanGoNext READ canGoNext CONSTANT)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious CONSTANT)
    Q_PROPERTY(bool CanPlay READ canPlay CONSTANT)
    Q_PROPERTY(bool CanPause READ canPause CONSTANT)
    Q_PROPERTY(bool CanSeek READ canSeek CONSTANT)
    Q_PROPERTY(bool CanControl READ canControl CONSTANT)

public:
    explicit MprisPlayer(LinuxMediaIntegration* owner);

    [[nodiscard]] QString     playbackStatus() const;
    [[nodiscard]] QVariantMap metadata() const;
    [[nodiscard]] double      volume() const;
    void                      setVolume(double gain);
    [[nodiscard]] qlonglong   position() const;

    /// Fixed at 1. The time-stretching DSP stages Cog uses to vary this are
    /// deliberately not ported, so a range would advertise a control that does
    /// nothing. Minimum and maximum both 1 is how MPRIS says "not adjustable".
    [[nodiscard]] double rate() const { return 1.0; }
    [[nodiscard]] double minimumRate() const { return 1.0; }
    [[nodiscard]] double maximumRate() const { return 1.0; }

    [[nodiscard]] bool canGoNext() const { return true; }
    [[nodiscard]] bool canGoPrevious() const { return true; }
    [[nodiscard]] bool canPlay() const { return true; }
    [[nodiscard]] bool canPause() const { return true; }
    [[nodiscard]] bool canSeek() const { return true; }
    [[nodiscard]] bool canControl() const { return true; }

public Q_SLOTS:
    void Play();
    void Pause();
    void PlayPause();
    void Stop();
    void Next();
    void Previous();

    /// Relative, in microseconds, and may be negative.
    void Seek(qlonglong offsetMicros);

    /// Absolute, in microseconds, for a named track.
    void SetPosition(const QDBusObjectPath& trackId, qlonglong positionMicros);

    /// Add and play a URL. Implemented rather than omitted because
    /// SupportedUriSchemes above claims `file`, and a scheme advertised without
    /// this method is a promise the player does not keep.
    void OpenUri(const QString& uri);

Q_SIGNALS:
    void Seeked(qlonglong positionMicros);

private:
    LinuxMediaIntegration* owner_;
};

// ---------------------------------------------------------------------------

class LinuxMediaIntegration final : public MediaIntegration {
public:
    explicit LinuxMediaIntegration(QObject* parent) : MediaIntegration(parent) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            // No session bus: a headless session, or a container without one.
            // Everything below then degrades to the base class's do-nothing
            // behaviour, which is why the adaptors are only built on success.
            return;
        }

        // Parented to this object, which is both what owns them and what makes
        // the connection find them. The root adaptor is never touched again --
        // its properties are constants and its two methods only forward -- so it
        // is created and left to the parent rather than held.
        new MprisRoot(this);
        player_ = new MprisPlayer(this);

        // ExportAdaptors, not ExportAllContents: the adaptors carry the D-Bus
        // interfaces, and exporting this object's own members as well would put
        // the internals of a MediaIntegration on the bus under a third interface.
        if (!bus.registerObject(QLatin1String(kObjectPath), this,
                                QDBusConnection::ExportAdaptors)) {
            return;
        }

        // The name is claimed *after* the object is exported, and the order
        // matters: the name appearing is the event that tells clients to go and
        // read the object, so a client that reacts at once must not find nothing
        // there.
        //
        // Suffixed with the process id, which the spec allows. Two players are
        // legal, and a stale unsuffixed name left by a crashed process would
        // otherwise make the *new* process the one without MPRIS.
        const QString service =
            QStringLiteral("org.mpris.MediaPlayer2.xpcog.instance%1")
                .arg(QCoreApplication::applicationPid());
        registered_ = bus.registerService(service);
    }

    void setNowPlaying(const NowPlayingInfo& info) override {
        info_ = info;
        // A fresh id per track, so a SetPosition aimed at the previous one can be
        // told apart from one aimed at this.
        trackSerial_ += 1;
        artUrl_ = writeArtwork(info.artwork);
        if (player_ != nullptr) {
            notify(QStringLiteral("Metadata"), player_->metadata());
        }
    }

    void setPlaybackState(bool playing, bool paused, double position) override {
        const bool statusChanged = playing != playing_ || paused != paused_;
        playing_  = playing;
        paused_   = paused;
        position_ = position;

        // Only the status. See the file comment for why Position is not announced.
        if (statusChanged && player_ != nullptr) {
            notify(QStringLiteral("PlaybackStatus"), player_->playbackStatus());
        }
    }

    void clear() override {
        info_ = {};
        artUrl_.clear();
        playing_  = false;
        paused_   = false;
        position_ = 0.0;
        if (player_ != nullptr) {
            notify(QStringLiteral("PlaybackStatus"), player_->playbackStatus());
            notify(QStringLiteral("Metadata"), player_->metadata());
        }
    }

    void setVolume(float gain) override {
        volume_ = gain;
        notify(QStringLiteral("Volume"), static_cast<double>(gain));
    }

    // --- read by the adaptors ---------------------------------------------

    [[nodiscard]] const NowPlayingInfo& info() const { return info_; }
    [[nodiscard]] const QString&        artUrl() const { return artUrl_; }
    [[nodiscard]] bool                  isPlaying() const { return playing_; }
    [[nodiscard]] bool                  isPaused() const { return paused_; }
    [[nodiscard]] double positionSeconds() const { return position_; }
    [[nodiscard]] float  volumeGain() const { return volume_; }

    [[nodiscard]] QDBusObjectPath trackPath() const {
        return QDBusObjectPath(
            QStringLiteral("/co/losno/xpcog/track/%1").arg(trackSerial_));
    }

    /// Emits Seeked, the one position signal MPRIS does want: it tells clients
    /// their extrapolation is now wrong and to re-read Position.
    void announceSeek(double seconds) {
        position_ = seconds;
        if (player_ != nullptr) {
            Q_EMIT player_->Seeked(
                static_cast<qlonglong>(seconds * kMicrosPerSecond));
        }
    }

    // The adaptors ask for these rather than emitting the base class's signals
    // themselves. Signals are technically public in Qt, so the direct form would
    // compile -- but "who can emit this" is worth keeping to one class.
    void requestRaise() { Q_EMIT raiseRequested(); }
    void requestQuit() { Q_EMIT quitRequested(); }
    void requestPlay() { Q_EMIT playRequested(); }
    void requestPause() { Q_EMIT pauseRequested(); }
    void requestPlayPause() { Q_EMIT playPauseRequested(); }
    void requestStop() { Q_EMIT stopRequested(); }
    void requestNext() { Q_EMIT nextRequested(); }
    void requestPrevious() { Q_EMIT previousRequested(); }
    void requestSeek(double seconds) { Q_EMIT seekRequested(seconds); }
    void requestVolume(float gain) { Q_EMIT volumeRequested(gain); }
    void requestOpenUrl(const QUrl& url) { Q_EMIT openUrlRequested(url); }

private:
    /// PropertiesChanged, for one property.
    ///
    /// Hand-built because Qt supplies no helper: QDBusAbstractAdaptor exports
    /// properties but never announces a change to one.
    void notify(const QString& property, const QVariant& value) {
        if (!registered_) {
            return;
        }
        QDBusMessage signal = QDBusMessage::createSignal(
            QLatin1String(kObjectPath), QLatin1String(kPropertiesIface),
            QStringLiteral("PropertiesChanged"));
        signal << QString::fromLatin1(kPlayerInterface)
               << QVariantMap{{property, value}} << QStringList{};
        QDBusConnection::sessionBus().send(signal);
    }

    /// Artwork has to be a URL, so it has to be a file.
    ///
    /// MPRIS carries mpris:artUrl, not image bytes -- unlike SMTC, which takes a
    /// stream, and macOS, which takes an NSImage. One path, rewritten per track,
    /// rather than a temporary file each: the latter accumulates for the life of
    /// the process, and a long listening session is thousands of tracks.
    ///
    /// Rewriting in place has one consequence worth stating: a client caching by
    /// URL sees the same URL with different contents. In practice clients re-read
    /// on the Metadata signal, and the alternative trades that for the leak.
    [[nodiscard]] static QString writeArtwork(const QImage& artwork) {
        if (artwork.isNull()) {
            return {};
        }
        const QString directory =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (directory.isEmpty() || !QDir().mkpath(directory)) {
            return {};
        }
        const QString path = directory + QStringLiteral("/mpris-art.png");
        if (!artwork.save(path, "PNG")) {
            return {};
        }
        return QUrl::fromLocalFile(path).toString();
    }

    MprisPlayer* player_     = nullptr;
    bool         registered_ = false;

    NowPlayingInfo info_;
    QString        artUrl_;
    bool           playing_     = false;
    bool           paused_      = false;
    double         position_    = 0.0;
    float          volume_      = 1.0F;
    quint64        trackSerial_ = 0;
};

// --- MprisRoot ------------------------------------------------------------

MprisRoot::MprisRoot(LinuxMediaIntegration* owner)
    : QDBusAbstractAdaptor(owner), owner_(owner) {}

void MprisRoot::Raise() { owner_->requestRaise(); }
void MprisRoot::Quit() { owner_->requestQuit(); }

// --- MprisPlayer ----------------------------------------------------------

MprisPlayer::MprisPlayer(LinuxMediaIntegration* owner)
    : QDBusAbstractAdaptor(owner), owner_(owner) {}

QString MprisPlayer::playbackStatus() const {
    if (!owner_->isPlaying()) {
        return QStringLiteral("Stopped");
    }
    return owner_->isPaused() ? QStringLiteral("Paused") : QStringLiteral("Playing");
}

QVariantMap MprisPlayer::metadata() const {
    const NowPlayingInfo& info = owner_->info();

    QVariantMap map;
    // Required even when nothing is playing: a client that finds no trackid
    // treats the whole map as invalid.
    map.insert(QStringLiteral("mpris:trackid"),
               QVariant::fromValue(owner_->trackPath()));

    if (info.duration > 0.0) {
        // Omitted rather than zero when unknown, which is how a stream reports
        // itself. A zero length makes a client draw a seek bar of no width, and
        // some refuse to seek at all.
        map.insert(QStringLiteral("mpris:length"),
                   static_cast<qlonglong>(info.duration * kMicrosPerSecond));
    }
    if (!info.title.isEmpty()) {
        map.insert(QStringLiteral("xesam:title"), info.title);
    }
    if (!info.artist.isEmpty()) {
        // A list, not a string: xesam:artist is defined as an array, and a client
        // reading it as one gets an empty artist out of a bare string.
        map.insert(QStringLiteral("xesam:artist"), QStringList{info.artist});
    }
    if (!info.album.isEmpty()) {
        map.insert(QStringLiteral("xesam:album"), info.album);
    }
    if (!owner_->artUrl().isEmpty()) {
        map.insert(QStringLiteral("mpris:artUrl"), owner_->artUrl());
    }
    return map;
}

double MprisPlayer::volume() const {
    return static_cast<double>(owner_->volumeGain());
}

void MprisPlayer::setVolume(double gain) {
    owner_->requestVolume(static_cast<float>(std::clamp(gain, 0.0, 1.0)));
}

qlonglong MprisPlayer::position() const {
    return static_cast<qlonglong>(owner_->positionSeconds() * kMicrosPerSecond);
}

void MprisPlayer::Play() { owner_->requestPlay(); }
void MprisPlayer::Pause() { owner_->requestPause(); }
void MprisPlayer::PlayPause() { owner_->requestPlayPause(); }
void MprisPlayer::Stop() { owner_->requestStop(); }
void MprisPlayer::Next() { owner_->requestNext(); }
void MprisPlayer::Previous() { owner_->requestPrevious(); }

void MprisPlayer::Seek(qlonglong offsetMicros) {
    // Clamped at zero rather than allowed to go negative: MPRIS says a seek past
    // the start lands at the start, not that it fails.
    const double target =
        std::max(0.0, owner_->positionSeconds() +
                          (static_cast<double>(offsetMicros) / kMicrosPerSecond));
    owner_->announceSeek(target);
    owner_->requestSeek(target);
}

void MprisPlayer::SetPosition(const QDBusObjectPath& trackId,
                              qlonglong              positionMicros) {
    // Drop a request aimed at a track that is no longer playing. The spec
    // requires this, and the race is real: a panel's seek bar sends the position
    // the user released the mouse on, and by then the track may have ended --
    // applying it would jump the *next* track to a position in the previous one.
    if (trackId != owner_->trackPath()) {
        return;
    }
    const double target =
        std::max(0.0, static_cast<double>(positionMicros) / kMicrosPerSecond);
    owner_->announceSeek(target);
    owner_->requestSeek(target);
}

void MprisPlayer::OpenUri(const QString& uri) {
    // Validated here rather than passed straight through. This is the one method
    // that takes an arbitrary string from any process on the bus, and the scheme
    // list says `file` -- so anything else is refused instead of being handed to
    // the playlist to fail on later, where the cause would be invisible.
    const QUrl url(uri, QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile()) {
        return;
    }
    owner_->requestOpenUrl(url);
}

MediaIntegration* MediaIntegration::create(QObject* parent) {
    return new LinuxMediaIntegration(parent);
}

}  // namespace xpcog::platform

#include "LinuxMediaIntegration.moc"
