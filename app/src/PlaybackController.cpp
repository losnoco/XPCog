#include "PlaybackController.hpp"

#include "Text.hpp"

#include <wx/translation.h>

#include "xpcog/core/audio/PanelFeed.hpp"
#include "xpcog/platform/CrashReporter.hpp"

#include <algorithm>
#include <utility>

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

bool PlaybackController::busy() const {
    return starting_.load(std::memory_order_acquire) ||
           stopping_.load(std::memory_order_acquire);
}

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
    // A fresh gesture, so the track begins at its top rather than wherever the
    // last one was.
    resumeAt_ = 0.0;
    requestStart(id);
}

void PlaybackController::requestStart(TrackId id, Search hunt) {
    // Set on every start, not only on the ones that want it: a start that is not
    // part of a search is a gesture that ended whatever search was running.
    //
    // The list goes with the direction. Leaving it behind would outlive the
    // search it belongs to, and next() and previous() read it to decide where to
    // carry on from -- so a stale one sends the following search off from
    // wherever an abandoned one happened to stop.
    searching_ = hunt;
    if (searching_ == Search::None) {
        searched_.clear();
    }

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
        endSearch(/*found=*/true);
        clearFailure(id);
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
    markFailure(id);
    playbackFailed.publish(id, toUtf8(_("No decoder could open this file")));

    // A track that was *named* stops here, and that is the whole of it: a
    // double-click asks for one entry, and answering it by playing a different
    // one is not an answer. Next and Previous are the exceptions, because "the
    // next one that works" is what those buttons mean -- and they say so by
    // asking for the hunt, each in their own direction.
    if (searching_ != Search::None) {
        const Search direction = searching_;
        searched_.push_back(id);
        const bool stepped = direction == Search::Backward ? playlist_.previous()
                                                           : playlist_.next();
        if (stepped) {
            const TrackId following = playlist_.current().value_or(kInvalidTrackId);
            if (std::find(searched_.begin(), searched_.end(), following) ==
                searched_.end()) {
                requestStart(following, direction);
                return;
            }
        }
        // Either the playlist ran out, or it repeated and handed back something
        // already tried -- a full lap with nothing playable on it.
        endSearch(/*found=*/false);
    }

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

    // Pausing during a quiet search ends it. The search is only allowed to run
    // because something is playing under it; a listener who has just stopped the
    // sound is not expecting a track they never chose to start a moment later.
    cancelSearch();

    if (paused_) {
        engine_->resume();
        paused_ = false;
    } else {
        engine_->pause();
        paused_ = true;
    }
    publishState();
}

void PlaybackController::cancelSearch() {
    if (searching_ == Search::None && searched_.empty()) {
        return;
    }
    // The bump is the cancel: a candidate already being opened comes back with a
    // generation nobody is waiting for, and is dropped before it is read.
    ++startGeneration_;
    searching_ = Search::None;
    searched_.clear();
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
    searching_ = Search::None;
    searched_.clear();

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
    // The quiet search, when there is something playing for it to be quiet
    // *about*. It looks ahead without moving the current entry, so the interface
    // stays on the track being heard rather than following the search through
    // rows that turn out to be dead.
    if (settings_.KeepPlayingWhileSkipping() && playing() && !paused_) {
        // Pressed again while a search is already running the same way: carry
        // that one on from where it had reached rather than starting it over.
        // Starting over would re-open the entry it is currently stuck on, which
        // is the one thing an impatient second press must not mean. `searched_`
        // is non-empty only while a search is in flight -- both kinds clear it
        // when they end -- and during a quiet one nothing is playing but the
        // track the search began under. A search running the *other* way is not
        // carried on: its position is somewhere the button was not pressed
        // towards, so this one starts from the track being heard.
        const TrackId from = searching_ == Search::Forward && !searched_.empty()
                                 ? searched_.back()
                                 : playlist_.current().value_or(kInvalidTrackId);
        if (from != kInvalidTrackId) {
            startProbe(from, Search::Forward);
            return;
        }
    }

    searched_.clear();
    if (!playlist_.next()) {
        stop();
        return;
    }
    // Not playTrack(): this start is allowed to keep looking, and playTrack() is
    // the gesture that names one entry and means it.
    resumeAt_ = 0.0;
    requestStart(*playlist_.current(), Search::Forward);
}

void PlaybackController::previous() {
    // A backwards search already running is carried on, and that test comes
    // before the clock. The search began with a press near the top of a track,
    // and during the quiet shape that track has gone on playing underneath it --
    // so by the second press the clock reads well past the threshold, and
    // answering with "restart this one" would be answering a question about the
    // previous track with the current one.
    const bool continuing = searching_ == Search::Backward && !searched_.empty();

    // Cog restarts the track when you are more than a few seconds in, which is
    // what every other player does and what a user pressing Previous mid-song
    // means. Only near the start does it step back a track.
    constexpr double kRestartThreshold = 3.0;
    if (!continuing && playing() && position() > kRestartThreshold) {
        seek(0.0);
        return;
    }

    // The quiet search, walking the other way. Everything next() does, and for
    // the same reasons -- see the block above.
    if (settings_.KeepPlayingWhileSkipping() && playing() && !paused_) {
        const TrackId from = continuing
                                 ? searched_.back()
                                 : playlist_.current().value_or(kInvalidTrackId);
        if (from != kInvalidTrackId) {
            startProbe(from, Search::Backward);
            return;
        }
    }

    searched_.clear();
    if (!playlist_.previous()) {
        // Unlike Next, which stops: running off the top of the playlist is not
        // the end of it, and Cog's -prev leaves what is playing alone there.
        return;
    }
    // Not playTrack(): this start is allowed to keep looking backwards, and
    // playTrack() is the gesture that names one entry and means it.
    resumeAt_ = 0.0;
    requestStart(*playlist_.current(), Search::Backward);
}

// --- the quiet search ---------------------------------------------------
//
// Same shape as the loud one, and deliberately: one candidate per task on the
// same executor, each checking the generation before it does any work. A search
// written as a single long task would be uninterruptible in the way that
// matters -- the executor is serial, so a track clicked mid-search would sit
// behind it, and on the share that made all this necessary that is a wait of
// minutes.
//
// What it cannot interrupt is an open already under way. A source that is
// waiting out its own timeout is not reachable from here, so a cancel takes
// effect at the next candidate rather than at once.

void PlaybackController::startProbe(TrackId from, Search direction) {
    searching_ = direction;
    statusNote.publish(toUtf8(_("Looking for a playable track...")));
    probeNext(from, ++startGeneration_);
}

void PlaybackController::probeNext(TrackId from, std::uint64_t generation) {
    // The direction is read from the search rather than carried through the
    // hop, and it is safe to: the generation check is what says this call still
    // belongs to the search that set it, and anything that ends one clears it.
    const auto candidate = searching_ == Search::Backward
                               ? playlist_.peekPrevious(from)
                               : playlist_.peekNext(from);
    if (!candidate || std::find(searched_.begin(), searched_.end(), *candidate) !=
                          searched_.end()) {
        endSearch(/*found=*/false);
        return;
    }
    const PlaylistEntry* entry = playlist_.find(*candidate);
    if (entry == nullptr) {
        endSearch(/*found=*/false);
        return;
    }

    searched_.push_back(*candidate);
    const TrackId id  = *candidate;
    const Url     url = entry->url;

    starter_->post([this, id, url, generation] {
        bool opens = false;
        if (generation == startGeneration_.load()) {
            // Opened and thrown away. This is exactly the test the engine
            // applies -- AudioEngine::openTrack() begins with the same call, and
            // everything after it is bookkeeping -- so a candidate that passes
            // here is one that will play. The cost is opening it twice, which
            // for a file is nothing and for a stream is one connection made and
            // dropped; the alternative is holding a source open across a search
            // that may pass a hundred of them.
            opens = static_cast<bool>(registry_.open(url));
        }
        dispatch_([this, id, opens, generation] { finishProbe(id, opens, generation); });
    });
}

void PlaybackController::finishProbe(TrackId id, bool opens, std::uint64_t generation) {
    // Superseded: something else was asked for while this candidate was being
    // opened, and it owns the transport now.
    if (generation != startGeneration_.load()) {
        return;
    }

    if (opens) {
        endSearch(/*found=*/true);
        // Through playTrack(), so what happens from here is an ordinary start of
        // an ordinary track: the search is over, and its result is not a special
        // kind of playing.
        playTrack(id);
        return;
    }

    markFailure(id);
    playbackFailed.publish(id, toUtf8(wxString::Format(_("Could not play %s"),
                                                       toWx(nameOf(id, {})))));
    probeNext(id, generation);
}

void PlaybackController::endSearch(bool found) {
    searching_ = Search::None;
    searched_.clear();
    if (!found) {
        statusNote.publish(toUtf8(_("No playable track found")));
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
                // The share came back, or the file did. A row marked unplayable
                // by an earlier failure has just played, so the mark is stale.
                clearFailure(audible_);
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
        TrackId id = kInvalidTrackId;
        for (std::size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.at(i).url.toString() == name) {
                id = playlist_.at(i).id;
                markFailure(id);
                break;
            }
        }
        playbackFailed.publish(id, toUtf8(wxString::Format(_("Could not play %s"),
                                                           toWx(nameOf(id, name)))));
    });
}

std::string PlaybackController::nameOf(TrackId id, const std::string& fallback) const {
    // What the row says, which is the tag title or the file name. The URL is the
    // fallback rather than the answer: a listener told "could not play
    // file:///Users/.../04%20Untitled.flac" has been told the truth in the least
    // useful available form, and a stream that is not in the playlist is the only
    // case with nothing better to offer.
    const PlaylistEntry* entry = playlist_.find(id);
    return entry != nullptr ? entry->title() : fallback;
}

// --- the unplayable mark ------------------------------------------------
//
// The same flag the library scanner sets, and it is read the same way: the model
// greys the row and puts a warning glyph in the status column. Set from playback
// as well as from scanning because the two find different things -- the scanner
// says a file has no decoder, this says the file was not there when it was
// wanted, which is what a share going away looks like.

void PlaybackController::markFailure(TrackId id) {
    if (playlist_.find(id) == nullptr) {
        return;
    }
    // Through update() rather than by writing to the entry, so the playlist's
    // own change notification fires and the row redraws.
    //
    // The stored sentence is English and stays English: the library persists this
    // field, and what goes in a database is not what a window says. The
    // translated sentence is the one published to the status line.
    playlist_.update(id, [](PlaylistEntry& entry) {
        entry.error        = true;
        entry.errorMessage = "could not be opened for playback";
    });
    trackMetadataChanged.publish(id);
}

void PlaybackController::clearFailure(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr || !entry->error) {
        return;
    }
    playlist_.update(id, [](PlaylistEntry& target) {
        target.error = false;
        target.errorMessage.clear();
    });
    trackMetadataChanged.publish(id);
}

}  // namespace xpcog::app
