#include "PlaybackController.hpp"

#include "Text.hpp"

#include <wx/translation.h>

#include "xpcog/core/audio/PanelFeed.hpp"
#include "xpcog/platform/CrashReporter.hpp"

#include <algorithm>
#include <utility>
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

PlaybackController::PlaybackController(const PluginRegistry& registry,
                                       Playlist& playlist, Settings& settings,
                                       Dispatcher dispatch)
    : registry_(registry),
      playlist_(playlist),
      settings_(settings),
      dispatch_(std::move(dispatch)),
      ring_(kRingSamples),
      ticker_(&sink_) {
    output_ = makeMiniaudioOutput(ring_);
    // Attached for the life of the output, whether or not anything is drawing it.
    // The cost when nothing is looking is one atomic load and a mono mixdown per
    // callback, which is far below the cost of attaching and detaching it as a
    // panel is shown and hidden -- and that would have to be done from the
    // interface's thread while the callback reads the pointer.
    output_->setTap(&tap_);
    engine_ = std::make_unique<AudioEngine>(registry_, *output_, ring_, settings_);
    engine_->setDelegate(this);
    engine_->setVolume(static_cast<float>(settings_.Volume()));

    starter_ = std::make_unique<SerialExecutor>();

    sink_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        positionChanged.publish(position(), duration());
    });
}

PlaybackController::~PlaybackController() {
    // The executor calls into the engine and reports back through this object, so
    // it has to be finished before either is torn down. A start that is
    // mid-connect delays this by up to the source's own header timeout; bumping
    // the generation first means it returns without doing the work.
    ++startGeneration_;
    starter_.reset();

    // Before the engine goes: its delegate is this object, and a callback
    // arriving mid-destruction would touch a half-dead object.
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
    // And a stop in flight is not playing any more. Same reason, from the other
    // side: stop() posts engine_->stop() and returns, so the engine's status is
    // still Playing for as long as the fade and the two joins take. Answering
    // from it there would report playback that the listener has already ended --
    // which is what left the spectrum's clock running after a manual stop, while
    // a track ending by itself came back through stoppedNaturally() with the
    // status already settled and switched it off correctly.
    if (stopping_.load()) {
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

double PlaybackController::playedSeconds() const {
    if (starting_.load()) {
        return 0.0;
    }
    return engine_ ? engine_->playedSeconds() : 0.0;
}

double PlaybackController::duration() const {
    const PlaylistEntry* entry = playlist_.find(audible_);
    return (entry != nullptr) ? entry->duration() : 0.0;
}

TrackId PlaybackController::currentTrack() const { return audible_; }

void PlaybackController::publishState() {
    playbackStateChanged.publish(playing(), paused_);
    if (playing() && !paused_) {
        ticker_.Start(kTickMs);
    } else {
        ticker_.Stop();
    }
}

// --- transport ----------------------------------------------------------

void PlaybackController::reopenOutput() {
    const TrackId current = currentTrack();
    if (current == kInvalidTrackId || !playing()) {
        return;
    }

    // Under the running stream where that is possible. The decoded audio is
    // already converted for the format the device is running, so a device that
    // will run the same format can simply take it over -- no re-open, no seek,
    // and nothing lost across the seam. The engine answers false only when there
    // is nothing to move (paused, most often); a switch that is accepted and then
    // fails comes back through outputSwitchFailed().
    if (engine_ && engine_->switchOutputDevice()) {
        return;
    }

    restartForOutputChange();
}

void PlaybackController::resumeTrack(TrackId id, double seconds, bool startPaused) {
    if (playlist_.find(id) == nullptr) {
        return;
    }
    // The same route restartForOutputChange() takes, and for the same reason:
    // resumeAt_ survives requestStart() and is applied by finishStart() at the
    // first moment a seek is allowed.
    failedStarts_.clear();
    resumeAt_ = std::max(0.0, seconds);
    requestStart(id);

    if (startPaused) {
        // After the request, not before: pausing an engine that is being rebuilt
        // is not a defined thing to do, and playPause() declines while a start is
        // in flight for that reason. The flag is set here and the engine is told
        // once the track is actually open -- see finishStart().
        pauseOnStart_ = true;
    }
}

void PlaybackController::restartForOutputChange() {
    const TrackId current = currentTrack();
    if (current == kInvalidTrackId) {
        return;
    }
    // Not playTrack(): the resume position has to survive the start, and
    // playTrack() clears it because an ordinary gesture begins at the top.
    failedStarts_.clear();
    resumeAt_ = position();
    requestStart(current);
}

void PlaybackController::outputSwitchFailed() {
    // On the feeder thread, like every other delegate call. The restart touches
    // the playlist and the engine's lifecycle, both of which belong to the
    // interface's thread.
    dispatch_([this] { restartForOutputChange(); });
}

void PlaybackController::playTrack(TrackId id) {
    // A fresh gesture, so the record of what has already failed starts empty and
    // the track begins at its top rather than wherever the last one was.
    failedStarts_.clear();
    resumeAt_ = 0.0;
    requestStart(id);
}

void PlaybackController::requestStart(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        // Cog's "Invalid playlist entry reached" (PlaybackController.m:863), and
        // reported for Cog's reason: something asked to play a track the playlist
        // does not have, which is a bug here rather than anything the listener
        // did. The transport then does nothing, which is safe and says nothing.
        //
        // A file that no decoder opens is deliberately *not* reported alongside
        // it. That is a user dropping a .txt on the window, and Cog's own note --
        // "captureMessage is too spammy to use for anything but actual errors"
        // (PlaybackController.m:24) -- is about exactly that distinction.
        platform::reportProblem("Invalid playlist entry reached");
        return;
    }

    const Url url = entry->url;
    playlist_.setCurrent(id);
    paused_ = false;

    // Nothing is audible until the answer comes back, and the ticker must not
    // read a device that is being reconfigured underneath it.
    audible_ = kInvalidTrackId;
    ticker_.Stop();
    starting_.store(true);

    const std::uint64_t generation = ++startGeneration_;
    startPending.publish(id);
    playbackStateChanged.publish(false, false);

    starter_->post([this, id, url, generation] {
        bool opened = false;
        // A newer request landed while this one waited its turn. Doing the work
        // anyway would open a source only to tear it down a moment later --
        // which for a URL means a whole connection made and dropped.
        if (generation == startGeneration_.load()) {
            engine_->stop();
            opened = engine_->play(url);
        }
        dispatch_([this, id, opened, generation] { finishStart(id, opened, generation); });
    });
}

void PlaybackController::finishStart(TrackId id, bool opened,
                                     std::uint64_t generation) {
    // Superseded: a newer request owns the engine and the state now, and this one
    // must not report a track as current that nothing is playing.
    if (generation != startGeneration_.load()) {
        return;
    }
    starting_.store(false);

    if (opened) {
        failedStarts_.clear();
        audible_ = id;
        currentTrackChanged.publish(audible_);
        publishState();

        // Here rather than beside the request, because seek() declines while a
        // start is in flight and this is the first moment it does not.
        if (resumeAt_ > 0.0) {
            const double target = resumeAt_;
            resumeAt_           = 0.0;
            seek(target);
        }

        // Likewise the pause a resumed session asked for: playPause() declines
        // while a start is in flight, so the request is held until here. After
        // the seek, so the track is paused at the position it resumed to rather
        // than at its top.
        if (pauseOnStart_) {
            pauseOnStart_ = false;
            playPause();
        }
        return;
    }

    resumeAt_ = 0.0;
    playbackFailed.publish(id, toUtf8(_("No decoder could open this file")));
    failedStarts_.push_back(id);

    // Cog does not stall the playlist on one bad file, and neither does this: ask
    // for the next one exactly as an end-of-track would. But nextForPlayback()
    // answers from the repeat rules, so with repeat on and nothing playable left
    // it offers the same entry for ever -- hence the memory of what has already
    // been tried this gesture.
    const auto following = playlist_.nextForPlayback();
    if (following.has_value() &&
        std::find(failedStarts_.begin(), failedStarts_.end(), *following) ==
            failedStarts_.end()) {
        requestStart(*following);
        return;
    }

    failedStarts_.clear();
    audible_ = kInvalidTrackId;
    currentTrackChanged.publish(audible_);
    publishState();
}

void PlaybackController::playPause() {
    // A start already in flight: the button press has nothing to act on yet, and
    // pausing an engine that is being rebuilt is not a defined thing to do.
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
    publishState();
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
    // Cancels a start in flight as well as stopping what is playing: bumping the
    // generation is what makes the executor drop its result instead of reporting
    // a track as current after the user asked for silence.
    ++startGeneration_;
    starting_.store(false);
    failedStarts_.clear();

    // Posted, because stop() blocks: it plays out the fade and joins two threads.
    // Short, but it is the same thread that draws the window.
    //
    // Which is why the flag is raised first: everything below publishes state,
    // and until that task has run the engine still reports itself as playing.
    // Cleared on the far side of the call, in the executor's order -- a start
    // queued behind this one cannot overtake it, so the clear cannot land after
    // a later play() and take that track's state down with it.
    stopping_.store(true);
    starter_->post([this] {
        engine_->stop();
        stopping_.store(false);
    });

    // So the visualisers go quiet with the audio rather than holding the last
    // thing they were given on screen. Only on an explicit stop: a gapless
    // advance never comes through here, and clearing between tracks would blank
    // a display watching audio that never actually stopped.
    //
    // The panel feed is cleared rather than merely flushed, and the difference
    // matters here. engine_->stop() is posted, so the decoder is still winding
    // down on its own thread and may post another state or two after this line;
    // dropping the frames alone would leave the track still marked audible and
    // those late arrivals would put the panel back on screen. Forgetting which
    // track is audible is what makes them unreachable. trackBegan() names one
    // again on the next start.
    tap_.clear();
    PanelFeed::instance().clear();
    paused_  = false;
    audible_ = kInvalidTrackId;
    currentTrackChanged.publish(audible_);
    publishState();
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
        positionChanged.publish(position(), duration());
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
// and nothing may take a lock the interface's thread holds -- hence the
// dispatcher and no more work than choosing what to say.

std::optional<Url> PlaybackController::nextTrack() {
    // Reading the playlist from this thread is safe only because the interface
    // does not mutate it during playback without stopping first. That is the one
    // rule this class relies on, and the reason playlist edits go through the
    // undo stack on the interface's thread rather than being touched here.
    const auto id = playlist_.nextForPlayback();
    if (!id) {
        return std::nullopt;
    }
    const PlaylistEntry* entry = playlist_.find(*id);
    return (entry != nullptr) ? std::optional{entry->url} : std::nullopt;
}

void PlaybackController::trackBegan(const Url& url) {
    // Said here rather than on the interface's thread below, because it is what
    // makes a front panel show the right track's display: across a gapless seam
    // two decoders are producing at once, and until this lands the queue does not
    // know which of them is being heard.
    PanelFeed::instance().setAudibleTrack(url);

    // The seam reached the speaker. Find which entry that was and tell the
    // interface; the dispatcher is what moves the work across.
    const std::string text = url.toString();
    dispatch_([this, text] {
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.at(i).url.toString() == text) {
                audible_ = playlist_.at(i).id;
                // setAudible, not setCurrent: this is the seam reaching the
                // speaker, and the engine asked what follows this track a
                // buffer's worth of audio ago. Resetting the read-ahead cursor
                // to here would have it answer with that same track again.
                playlist_.setAudible(audible_);
                currentTrackChanged.publish(audible_);
                publishState();
                return;
            }
        }
    });
}

void PlaybackController::streamMetadataChanged(const Url& url, const MetadataMap& tags) {
    // Copied across rather than referenced: the feeder thread owns neither the
    // playlist nor the map by the time the interface runs this.
    const std::string text = url.toString();
    dispatch_([this, text, tags] {
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.at(i).url.toString() != text) {
                continue;
            }
            // Through update() rather than by writing to the entry, so the
            // playlist's own change notification fires and the id index stays
            // intact.
            const TrackId id = playlist_.at(i).id;
            playlist_.update(id,
                             [&tags](PlaylistEntry& entry) { entry.applyMetadata(tags); });
            trackMetadataChanged.publish(id);
            return;
        }
    });
}

void PlaybackController::stoppedNaturally() {
    dispatch_([this] {
        audible_ = kInvalidTrackId;
        paused_  = false;
        currentTrackChanged.publish(audible_);
        publishState();
    });
}

void PlaybackController::trackFailed(const Url& url) {
    const std::string name = url.toString();
    dispatch_([this, name] {
        playbackFailed.publish(
            kInvalidTrackId,
            toUtf8(wxString::Format(_("Could not play %s"), toWx(name))));
    });
}

}  // namespace xpcog::app
