#include "PlaybackController.hpp"

#include "xpcog/core/audio/PanelFeed.hpp"

#include <QMetaObject>

#include <algorithm>
#include <vector>

namespace xpcog::app {
namespace {

/// The ring *after* the DSP chain, so this is deliberately shallow: about 186 ms
/// of stereo at 44.1 kHz. The depth that protects the device from a scheduling
/// hiccup is the engine's own buffer ahead of the chain (kPreRingSamples, Cog's
/// BUFFER_SIZE); see the AudioEngine class comment for why the two are separate.
///
/// This number is the latency of every DSP change. At Cog's 1<<18 it was three
/// seconds, and moving an equaliser slider appeared to do nothing at all. It only
/// has to be deep enough for the pump -- which runs a few hundred times faster
/// than playback and is fed by a three-second buffer -- to keep ahead of the
/// device across a timer tick.
constexpr std::size_t kRingSamples = 1U << 14;

/// Four updates a second. Cog's position field updates at a similar rate; going
/// faster only spends CPU redrawing a label that has not changed.
constexpr int kTickMs = 250;

}  // namespace

PlaybackController::PlaybackController(const PluginRegistry& registry, Playlist& playlist,
                                       Settings& settings, QObject* parent)
    : QObject(parent),
      registry_(registry),
      playlist_(playlist),
      settings_(settings),
      ring_(kRingSamples) {
    output_ = makeMiniaudioOutput(ring_);
    // Attached for the life of the output, whether or not anything is drawing it.
    // The cost when nothing is looking is one atomic load and a mono mixdown per
    // callback, which is far below the cost of attaching and detaching it as a panel
    // is shown and hidden -- and that would have to be done from the UI thread while
    // the callback reads the pointer.
    output_->setTap(&tap_);
    engine_ = std::make_unique<AudioEngine>(registry_, *output_, ring_, settings_);
    engine_->setDelegate(this);
    engine_->setVolume(static_cast<float>(settings_.Volume()));

    // Where the two blocking engine calls run. Not parented, because it is
    // moved to another thread and Qt requires a parentless object for that.
    starterThread_ = new QThread(this);
    starterThread_->setObjectName(QStringLiteral("xpcog-starter"));
    starter_ = new QObject;
    starter_->moveToThread(starterThread_);
    starterThread_->start();

    ticker_ = new QTimer(this);
    ticker_->setInterval(kTickMs);
    connect(ticker_, &QTimer::timeout, this, [this] {
        emit positionChanged(position(), duration());
    });
}

PlaybackController::~PlaybackController() {
    // The starter thread calls into the engine and reports back through this
    // object, so it has to be finished before either is torn down. A start that
    // is mid-connect delays this by up to the source's own header timeout;
    // bumping the generation first means it returns without doing the work.
    ++startGeneration_;
    if (starterThread_ != nullptr) {
        starterThread_->quit();
        starterThread_->wait();
    }
    delete starter_;
    starter_ = nullptr;

    // Before the engine goes: its delegate is this object, and a callback
    // arriving mid-destruction would touch a half-dead QObject.
    if (engine_) {
        engine_->setDelegate(nullptr);
        engine_->stop();
    }
}

// --- state --------------------------------------------------------------

bool PlaybackController::playing() const {
    // A start in flight is not playing yet, and the engine is being rebuilt
    // underneath it -- so this answers from the flag rather than from there.
    if (starting_.load()) {
        return false;
    }
    return engine_ && engine_->status() != PlaybackStatus::Stopped;
}

bool PlaybackController::paused() const { return paused_; }

double PlaybackController::position() const {
    if (starting_.load()) {
        return 0.0;
    }
    return engine_ ? engine_->trackPositionSeconds() : 0.0;
}

double PlaybackController::duration() const {
    const PlaylistEntry* entry = playlist_.find(audible_);
    return (entry != nullptr) ? entry->duration() : 0.0;
}

TrackId PlaybackController::currentTrack() const { return audible_; }

void PlaybackController::emitState() {
    emit playbackStateChanged(playing(), paused_);
    if (playing() && !paused_) {
        ticker_->start();
    } else {
        ticker_->stop();
    }
}

// --- transport ----------------------------------------------------------

void PlaybackController::playTrack(TrackId id) {
    // A fresh gesture, so the record of what has already failed starts empty.
    failedStarts_.clear();
    requestStart(id);
}

void PlaybackController::requestStart(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        return;
    }

    const Url url = entry->url;
    playlist_.setCurrent(id);
    paused_ = false;

    // Nothing is audible until the answer comes back, and the ticker must not
    // read a device that is being reconfigured underneath it.
    audible_ = kInvalidTrackId;
    ticker_->stop();
    starting_.store(true);

    const std::uint64_t generation = ++startGeneration_;
    emit startPending(id);
    emit playbackStateChanged(false, false);

    QMetaObject::invokeMethod(starter_, [this, id, url, generation] {
        bool opened = false;
        // A newer request landed while this one waited its turn. Doing the work
        // anyway would open a source only to tear it down a moment later --
        // which for a URL means a whole connection made and dropped.
        if (generation == startGeneration_.load()) {
            engine_->stop();
            opened = engine_->play(url);
        }
        QMetaObject::invokeMethod(this, [this, id, opened, generation] {
            finishStart(id, opened, generation);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void PlaybackController::finishStart(TrackId id, bool opened,
                                     std::uint64_t generation) {
    // Superseded: a newer request owns the engine and the state now, and this
    // one must not report a track as current that nothing is playing.
    if (generation != startGeneration_.load()) {
        return;
    }
    starting_.store(false);

    if (opened) {
        failedStarts_.clear();
        audible_ = id;
        emit currentTrackChanged(audible_);
        emitState();
        return;
    }

    emit playbackFailed(id, tr("No decoder could open this file"));
    failedStarts_.push_back(id);

    // Cog does not stall the playlist on one bad file, and neither does this:
    // ask for the next one exactly as an end-of-track would. But
    // nextForPlayback() answers from the repeat rules, so with repeat on and
    // nothing playable left it offers the same entry for ever -- hence the
    // memory of what has already been tried this gesture.
    const auto following = playlist_.nextForPlayback();
    if (following.has_value() &&
        std::find(failedStarts_.begin(), failedStarts_.end(), *following) ==
            failedStarts_.end()) {
        requestStart(*following);
        return;
    }

    failedStarts_.clear();
    audible_ = kInvalidTrackId;
    emit currentTrackChanged(audible_);
    emitState();
}

void PlaybackController::playPause() {
    // A start already in flight: the button press has nothing to act on yet,
    // and pausing an engine that is being rebuilt is not a defined thing to do.
    if (starting_.load()) {
        return;
    }

    if (!playing()) {
        // Nothing running: start the current entry, or the first one.
        const TrackId start = playlist_.current().value_or(kInvalidTrackId);
        if (playlist_.find(start) != nullptr) {
            playTrack(start);
        } else if (!playlist_.empty()) {
            playTrack(playlist_.at(0).id);
        }
        return;
    }

    if (paused_) {
        engine_->resume();
        paused_ = false;
    } else {
        engine_->pause();
        paused_ = true;
    }
    emitState();
}

void PlaybackController::reloadDsp() {
    if (engine_) {
        engine_->reloadDsp();
    }
}

double PlaybackController::sampleRate() const {
    // The device is renegotiated by play(); mid-start there is no answer worth
    // giving, and the spectrum treats zero as "not running yet".
    if (starting_.load()) {
        return 0.0;
    }
    return output_ ? output_->negotiatedFormat().sampleRate : 0.0;
}

void PlaybackController::stop() {
    // Cancels a start in flight as well as stopping what is playing: bumping
    // the generation is what makes the starter thread drop its result instead
    // of reporting a track as current after the user asked for silence.
    ++startGeneration_;
    starting_.store(false);
    failedStarts_.clear();

    // Queued, because stop() blocks: it plays out the fade and joins two
    // threads. Short, but it is the same thread that draws the window.
    QMetaObject::invokeMethod(starter_, [this] { engine_->stop(); },
                              Qt::QueuedConnection);

    // So the visualiser goes quiet with the audio rather than holding the last
    // window on screen. Only on an explicit stop: a gapless advance never comes
    // through here, and clearing between tracks would blank a display watching
    // audio that never actually stopped.
    tap_.clear();
    paused_  = false;
    audible_ = kInvalidTrackId;
    emit currentTrackChanged(audible_);
    emitState();
}

void PlaybackController::next() {
    if (playlist_.next()) {
        playTrack(*playlist_.current());
    } else {
        stop();
    }
}

void PlaybackController::previous() {
    // Cog restarts the track when you are more than a few seconds in, which is
    // what every other player does and what a user pressing Previous mid-song
    // means. Only near the start does it step back a track.
    constexpr double kRestartThreshold = 3.0;
    if (playing() && position() > kRestartThreshold) {
        seek(0.0);
        return;
    }

    if (playlist_.previous()) {
        playTrack(*playlist_.current());
    }
}

void PlaybackController::seek(double seconds) {
    if (starting_.load()) {
        return;
    }
    if (engine_) {
        static_cast<void>(engine_->seek(seconds));
        emit positionChanged(position(), duration());
    }
}

void PlaybackController::setVolume(double linear) {
    const double clamped = std::clamp(linear, 0.0, 1.0);
    engine_->setVolume(static_cast<float>(clamped));
    settings_.setVolume(clamped);
}

double PlaybackController::volume() const {
    return engine_ ? static_cast<double>(engine_->volume()) : 0.0;
}

// --- AudioEngine::Delegate ----------------------------------------------
//
// Everything below runs on the feeder thread. Nothing here may touch a widget,
// and nothing may take a lock the GUI thread holds -- hence queued signals and
// no more work than choosing what to say.

std::optional<Url> PlaybackController::nextTrack() {
    // Reading the playlist from this thread is safe only because the GUI thread
    // does not mutate it during playback without stopping first. That is the one
    // rule this class relies on and the reason playlist edits go through
    // PlaylistModel rather than being touched here.
    const auto id = playlist_.nextForPlayback();
    if (!id) {
        return std::nullopt;
    }
    const PlaylistEntry* entry = playlist_.find(*id);
    return (entry != nullptr) ? std::optional{entry->url} : std::nullopt;
}

void PlaybackController::trackBegan(const Url& url) {
    // Said here rather than on the GUI thread below, because it is what makes a
    // front panel show the right track's display: across a gapless seam two
    // decoders are producing at once, and until this lands the queue does not
    // know which of them is being heard.
    PanelFeed::instance().setAudibleTrack(url);

    // The seam reached the speaker. Find which entry that was and tell the GUI
    // thread; QueuedConnection is what moves the work across.
    const std::string text = url.toString();
    QMetaObject::invokeMethod(this, [this, text] {
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.at(i).url.toString() == text) {
                audible_ = playlist_.at(i).id;
                playlist_.setCurrent(audible_);
                emit currentTrackChanged(audible_);
                emitState();
                return;
            }
        }
    }, Qt::QueuedConnection);
}

void PlaybackController::streamMetadataChanged(const Url& url, const MetadataMap& tags) {
    // Copied across rather than referenced: the feeder thread owns neither the
    // playlist nor the map by the time the GUI thread runs this.
    const std::string text = url.toString();
    QMetaObject::invokeMethod(this, [this, text, tags] {
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.at(i).url.toString() != text) {
                continue;
            }
            // Through update() rather than by writing to the entry, so the
            // playlist's own change notification fires and the id index stays
            // intact.
            const TrackId id = playlist_.at(i).id;
            playlist_.update(id, [&tags](PlaylistEntry& entry) {
                entry.applyMetadata(tags);
            });
            emit trackMetadataChanged(id);
            return;
        }
    }, Qt::QueuedConnection);
}

void PlaybackController::stoppedNaturally() {
    QMetaObject::invokeMethod(this, [this] {
        audible_ = kInvalidTrackId;
        paused_  = false;
        emit currentTrackChanged(audible_);
        emitState();
    }, Qt::QueuedConnection);
}

void PlaybackController::trackFailed(const Url& url) {
    const QString name = QString::fromStdString(url.toString());
    QMetaObject::invokeMethod(this, [this, name] {
        emit playbackFailed(kInvalidTrackId, tr("Could not play %1").arg(name));
    }, Qt::QueuedConnection);
}

}  // namespace xpcog::app
