#include "Session.hpp"

#include "LastFmAccount.hpp"
#include "ListenBrainzAccount.hpp"
#include "RemoteToken.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/PlaylistFile.hpp"
#include "xpcog/core/library/Scanner.hpp"
#include "xpcog/core/scrobble/LastFmClient.hpp"
#include "xpcog/core/scrobble/ListenBrainzClient.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/FileManager.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace xpcog::app {
namespace {

/// The wall clock, in the units the library stores dates in.
[[nodiscard]] std::int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// What the status line says while a scan is running.
///
/// The two passes have two different things worth saying. While the scan is
/// still finding files there is no total to count against, and the names go past
/// far faster than the eye follows, so the folder they are in is what says how
/// far through a library the walk has got. Once it is reading tags the file
/// itself is the slow thing -- a decoder stuck on one track is the case this is
/// most worth having -- and the count is real.
[[nodiscard]] std::string scanActivityText(const ScanTask::Activity& activity) {
    const std::string name = activity.url.fileName();

    if (activity.phase == Scanner::Phase::Finding) {
        std::string where = name;
        if (const auto path = activity.url.localPath(); path && path->has_parent_path()) {
            where = pathToUtf8(path->parent_path().filename());
        }
        if (where.empty()) {
            return tr("Looking for files\xE2\x80\xA6");
        }
        return trf("Looking for files in %s\xE2\x80\xA6", where);
    }

    if (activity.total > 0) {
        return trf("Reading %s \xE2\x80\x94 %d of %d", name, activity.done, activity.total);
    }
    return trf("Reading %s", name);
}

}  // namespace

// --- construction ---------------------------------------------------------

Session::Session(const PluginRegistry& registry, Settings& settings, Dispatcher dispatch,
                 Options options)
    : registry_(registry),
      settings_(settings),
      dispatch_(std::move(dispatch)),
      view_(playlist_),
      commands_(playlist_, undo_, nullptr) {
    playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
    playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
    playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());

    // pathFromUtf8, not the std::filesystem::path constructor: that one reads a
    // std::string through the active code page on Windows, so a listener whose
    // profile is under a name CP-1252 cannot spell would get a queue beside a
    // directory that does not exist. See core/include/xpcog/core/FilePath.hpp.
    const std::filesystem::path libraryFile =
        options.dataDirectory.empty()
            ? pathFromUtf8(platform::libraryDatabasePath())
            : pathFromUtf8(options.dataDirectory) / "library.db";
    dataDirectory_ = libraryFile.parent_path();
    const std::filesystem::path cacheDirectory = options.cacheDirectory.empty()
                                                     ? pathFromUtf8(platform::cacheDirectory())
                                                     : pathFromUtf8(options.cacheDirectory);

    library_ = std::make_unique<Library>();
    if (!library_->open(pathToUtf8(libraryFile))) {
        // A library that will not open is not fatal: the player still plays, it
        // just will not remember the playlist. Saying so once beats failing to
        // launch.
        setStatus(trf("Library unavailable: %s", library_->lastError()));
        // And reported, when there is consent to report it. This is the shape
        // Cog's captureMessage calls have -- a thing that should have worked and
        // did not, on a path that then carries on regardless, which is exactly
        // the kind nobody files a bug about because nothing appears to be wrong.
        platform::reportProblem("Library would not open: " + library_->lastError());
        library_.reset();
    }
    commands_.setLibrary(library_.get());

    playback_ = std::make_unique<PlaybackController>(registry_, playlist_, settings_,
                                                     dispatch_, std::move(options.makeOutput));

    wireScrobbling();
    wireLyrics();

    waveforms_ = std::make_unique<WaveformProvider>(
        registry_, WaveformCache{cacheDirectory / "waveforms"}, dispatch_);

    const auto observe = [this](auto& signal, auto handler) {
        subscriptions_.push_back(signal.connect(std::move(handler)));
    };

    observe(playback_->positionChanged,
            [this](double seconds, double duration) { onPositionChanged(seconds, duration); });
    observe(playback_->currentTrackChanged,
            [this](TrackId id) { onCurrentTrackChanged(id, ListenChange::Announced); });
    observe(playback_->playbackStateChanged,
            [this](bool playing, bool paused) { onPlaybackStateChanged(playing, paused); });
    observe(playback_->startPending, [this](TrackId id) {
        const PlaylistEntry* entry = playlist_.find(id);
        setStatus(entry != nullptr ? trf("Connecting to %s...", entry->title())
                                   : tr("Connecting..."));
    });
    // Already translated: PlaybackController is app-layer and publishes the
    // sentence it wants shown, not a code.
    observe(playback_->playbackFailed,
            [this](TrackId, const std::string& reason) { setStatus(reason); });
    observe(playback_->statusNote, [this](const std::string& note) { setStatus(note); });
    observe(playback_->trackMetadataChanged, [this](TrackId id) {
        // A stream renamed itself. The row redraws from the view's own
        // notification; what has to happen here is the desktop's card, the
        // scrobble and the title, all of which read the entry rather than the
        // change.
        if (id == currentTrack_) {
            onCurrentTrackChanged(id, ListenChange::NewSong);
        }
    });

    observe(waveforms_->updated(),
            [this](const Url& url, const std::shared_ptr<const WaveformSummary>& summary) {
                onWaveformUpdated(url, summary);
            });

    observe(undo_.changed, [this] { setStatus(statusSummary()); });
}

Session::~Session() {
    // The scan borrows the registry and the PluginCache, and the cache is a
    // member here, so the task has to go first -- otherwise its thread outlives
    // what it is reading from. ~ScanTask cancels and joins, so nothing is still
    // posting to the interface after this returns.
    scan_.reset();
    // The same for the waveform worker, which borrows the registry too.
    waveforms_.reset();
}

void Session::attachDesktop(void* nativeWindow) {
    media_   = platform::MediaIntegration::create(dispatch_, nativeWindow);
    taskbar_ = platform::TaskbarIntegration::create(nativeWindow);

    const auto observe = [this](auto& signal, auto handler) {
        subscriptions_.push_back(signal.connect(std::move(handler)));
    };

    // The media keys and the OS's now-playing widget drive the same commands
    // the buttons do, rather than reaching into the engine separately.
    observe(media_->playPauseRequested, [this] { playback_->playPause(); });
    observe(media_->playRequested, [this] {
        if (!playback_->playing() || playback_->paused()) {
            playback_->playPause();
        }
    });
    observe(media_->pauseRequested, [this] {
        if (playback_->playing() && !playback_->paused()) {
            playback_->playPause();
        }
    });
    observe(media_->stopRequested, [this] { playback_->stop(); });
    observe(media_->nextRequested, [this] { playback_->next(); });
    observe(media_->previousRequested, [this] { playback_->previous(); });
    observe(media_->seekRequested, [this](double seconds) { playback_->seek(seconds); });

    // MPRIS only, on Linux. The other two platforms never publish these, so
    // there is nothing to guard: a signal that is never sent costs a connection.
    observe(media_->raiseRequested, [this] { raiseRequested.publish(); });
    observe(media_->quitRequested, [this] { quitRequested.publish(); });
    observe(media_->volumeRequested, [this](float gain) {
        // Widened explicitly. MPRIS carries a float and setVolume() takes a
        // double, so the conversion happens either way; saying so is what keeps
        // -Wdouble-promotion quiet.
        const auto wide = static_cast<double>(gain);
        playback_->setVolume(wide);
        // And the frontend's slider, so the panel and the window cannot end up
        // showing different volumes.
        volumeChanged.publish(wide);
    });
    observe(media_->openUrlRequested, [this](const Url& url) { addUrls({url}); });
}

void Session::start() {
    if (library_ && library_->loadPlaylist(playlist_)) {
        setStatus(statusSummary());
        restorePlayback();
    }
    // Restoring the saved playlist is not an edit the user made, so it must not
    // be the first thing Undo offers to take back.
    undo_.clear();

    // Last, and after the playlist is loaded: a client that connects the instant
    // the port opens should find the player it is about to be told about, not an
    // empty one.
    applyRemoteSettings();
}

void Session::tick() { playback_->tick(); }

void Session::save() {
    // Where the current track had got to. A raw key rather than a settings.def
    // entry because it is this application's own state, like the window
    // geometry the frontend writes beside it -- Cog keeps the equivalent as a
    // Core Data attribute on the entry (`currentPosition`), which is not a
    // preference either.
    //
    // Which entry it belongs to needs no recording: the library already marks
    // the current one, and loadPlaylist restores it.
    settings_.setRawValue("xpcog.playback.position", std::to_string(playback_->position()));

    if (library_ && !library_->savePlaylist(playlist_)) {
        // Worth saying, but not worth refusing to close over.
        setStatus(trf("Could not save the playlist: %s", library_->lastError()));
    }
    settings_.sync();
    saved_ = true;
}

void Session::setStatus(std::string text) {
    lastStatus_ = std::move(text);
    status.publish(lastStatus_);
}

std::string Session::statusSummary() const {
    double total = 0.0;
    for (const PlaylistEntry& entry : playlist_.entries()) {
        total += entry.duration();
    }
    const std::size_t count = playlist_.size();
    return fmt(trn("%zu track \xE2\x80\x94 %s", "%zu tracks \xE2\x80\x94 %s", count), count,
               formatClock(total));
}

LyricsLookup* Session::lyricsLookup() noexcept {
    return settings_.EnableLrclib() ? lyricsLookup_.get() : nullptr;
}

// --- the scan queue ---------------------------------------------------------

void Session::addUrls(const std::vector<Url>& urls, int atRow) {
    if (urls.empty()) {
        return;
    }
    pendingScans_.push_back(ScanRequest{urls, atRow, /*reload=*/false, {}});
    pumpScanQueue();
}

void Session::reloadTracks(const std::vector<TrackId>& ids) {
    std::vector<Url> urls;
    for (const TrackId id : ids) {
        if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
            urls.push_back(entry->url);
        }
    }
    if (urls.empty()) {
        return;
    }
    // Through the same queue an added folder goes through, and that is not
    // incidental: the scans share one PluginCache, which is not synchronised, so
    // a reload starting while a folder scan runs is the race the queue exists to
    // prevent.
    pendingScans_.push_back(ScanRequest{std::move(urls), -1, /*reload=*/true, {}});
    pumpScanQueue();
}

void Session::cancelScans() {
    // Everything not yet started goes too: cancelling one folder of a dropped
    // batch and then watching the next one start is not what the button looks
    // like it does.
    pendingScans_.clear();
    if (scan_) {
        scan_->cancel();
    }
}

void Session::pumpScanQueue() {
    if (scan_ || pendingScans_.empty()) {
        return;
    }

    ScanRequest request = std::move(pendingScans_.front());
    pendingScans_.erase(pendingScans_.begin());

    // Remembered for the duration of this scan, so the progress and finish
    // handlers below can report to whoever started it over the API. Empty for a
    // scan the window started, and every use of it is guarded on that.
    scanJobId_ = request.jobId;

    // Read per scan rather than once, so unticking the box in Preferences
    // applies to the next folder added instead of the next launch.
    //
    // Cog skips .cue files while walking a folder when this is off
    // (PlaylistLoader.m:264-282), which is what stops a folder holding album.cue
    // beside album.flac from adding every track twice -- once through the cue
    // sheet and once as the whole file.
    Scanner::Options scanOptions;
    scanOptions.readCueSheets        = settings_.ReadCueSheetsInFolders();
    scanOptions.readPlaylists        = settings_.ReadPlaylistsInFolders();
    scanOptions.skipAppleDoubleFiles = settings_.SkipAppleDoubleFiles();

    scan_ = std::make_unique<ScanTask>(registry_, &cache_, std::move(request.inputs),
                                       dispatch_, scanOptions);

    subscriptions_.push_back(scan_->progress.connect([this](int done, int total) {
        if (!scanJobId_.empty()) {
            remoteJobs_.setRunning(scanJobId_, done, total);
        }
        // The same number on the taskbar button, which is what Cog puts on its
        // Dock tile -- its progress bar tracks PlaylistLoader, not the seek
        // position. Left alone while the total is still zero: a bar sitting at
        // zero reads as stalled, where no bar reads as "not started", which is
        // the truth.
        if (total > 0 && taskbar_) {
            taskbar_->setProgress(static_cast<double>(done) / total);
        }
        scanProgress.publish(done, total);
    }));

    subscriptions_.push_back(scan_->activity.connect([this](const ScanTask::Activity& activity) {
        setStatus(scanActivityText(activity));
    }));

    const int  atRow  = request.atRow;
    const bool reload = request.reload;
    subscriptions_.push_back(scan_->finished.connect(
        [this, atRow, reload](const std::vector<PlaylistEntry>& entries, bool cancelled) {
            // The task owns the thread it is still returning from, so it cannot
            // be destroyed from inside its own callback. Handing it to the event
            // loop to drop is what deleteLater() was doing.
            auto* finished = scan_.release();
            dispatch_([finished] { delete finished; });

            if (taskbar_) {
                taskbar_->clearProgress();
            }
            scanFinished.publish();

            if (!scanJobId_.empty()) {
                // Reported before the entries are inserted, because
                // addScannedEntries can start another scan through the queue and
                // that overwrites scanJobId_.
                if (cancelled) {
                    remoteJobs_.fail(scanJobId_, "cancelled");
                } else {
                    remoteJobs_.finish(scanJobId_, entries.size());
                }
                scanJobId_.clear();
            }

            if (reload) {
                applyReloadedEntries(entries);
            } else {
                addScannedEntries(entries, atRow, cancelled);
            }
            pumpScanQueue();
        }));

    scanStarted.publish();
    scan_->start();
}

void Session::addScannedEntries(std::vector<PlaylistEntry> entries, int atRow, bool cancelled) {
    if (entries.empty()) {
        setStatus(cancelled ? tr("Nothing was added.") : tr("Nothing playable was found."));
        return;
    }

    // Embedded covers go to the artwork table, which is content-addressed, and
    // leave a hash behind on the entry. Here because this is the first point at
    // which a scanned entry and the library are both in reach.
    //
    // The table exists precisely so an album's twelve tracks hold one copy of
    // their cover between them rather than twelve. A library with
    // high-resolution art persisted per track reached gigabytes, which was most
    // of what made saving and loading the playlist slow.
    if (library_) {
        const std::int64_t now = unixNow();
        for (PlaylistEntry& entry : entries) {
            static_cast<void>(library_->adoptArtwork(entry));

            // And the track's play count row, which is created here rather than
            // by the first play. "First seen" is the date the track entered the
            // library -- if nothing writes it until sixty seconds into a listen
            // then it names the wrong event, and a track added and never played
            // has no date at all.
            //
            // The count comes back the other way. A track re-added to the
            // playlist carries whatever has been counted against it, so its
            // history survives being removed and added again; the max is for
            // the Cog import, whose entries arrive already holding a tally that
            // the row may not have seen.
            if (const auto record = library_->noteFirstSeen(entry, now)) {
                entry.playCount = std::max(entry.playCount, record->count);
            }
        }
    }

    // The row a drop targeted may no longer exist: the scan took time and the
    // user could have edited the playlist meanwhile. insert() clamps, so this
    // lands at the end rather than nowhere.
    const std::size_t where = (atRow >= 0) ? static_cast<std::size_t>(atRow) : playlist_.size();

    commands_.insert(std::move(entries), where);

    setStatus(statusSummary());
}

void Session::applyReloadedEntries(std::vector<PlaylistEntry> entries) {
    if (entries.empty()) {
        setStatus(tr("Nothing could be read."));
        return;
    }

    std::unordered_map<std::string, TrackId> byUrl;
    byUrl.reserve(playlist_.size());
    for (const PlaylistEntry& entry : playlist_.entries()) {
        byUrl.emplace(entry.url.toString(), entry.id);
    }

    std::size_t updated = 0;
    for (PlaylistEntry& fresh : entries) {
        const auto found = byUrl.find(fresh.url.toString());
        if (found == byUrl.end()) {
            // A cue sheet that has grown a track since it was added, or a file
            // whose container now expands differently. Adding it here would be a
            // reload that quietly lengthens the playlist, so it is dropped.
            continue;
        }

        if (library_) {
            static_cast<void>(library_->adoptArtwork(fresh));
        }

        playlist_.update(found->second, [&fresh](PlaylistEntry& entry) {
            // Everything a file can answer for is replaced; everything the
            // playlist knows and the file does not is kept. Getting this
            // backwards is not visible -- a reload that reset the play count and
            // dropped the track out of the queue would look like it had worked.
            const std::int64_t playCount     = entry.playCount;
            const double       position      = entry.currentPosition;
            const bool         stopAfter     = entry.stopAfter;
            const std::int64_t shuffleIndex  = entry.shuffleIndex;
            const std::int32_t queuePosition = entry.queuePosition;
            const Url          url           = entry.url;

            entry = fresh;

            entry.url             = url;
            entry.playCount       = playCount;
            entry.currentPosition = position;
            entry.stopAfter       = stopAfter;
            entry.shuffleIndex    = shuffleIndex;
            entry.queuePosition   = queuePosition;
        });

        if (library_) {
            if (const PlaylistEntry* entry = playlist_.find(found->second); entry != nullptr) {
                static_cast<void>(library_->saveEntry(*entry));
            }
        }
        ++updated;
    }

    tracksUpdated.publish();
    setStatus(fmt(trn("Re-read %zu track.", "Re-read %zu tracks.", updated), updated));
}

std::string Session::startRemoteScan(std::vector<std::string>   urls,
                                     std::optional<std::size_t> at) {
    std::vector<Url> inputs;
    inputs.reserve(urls.size());
    for (const std::string& text : urls) {
        // A URL or a plain path, which is what the CLI accepts too -- a client
        // that has a filename should not have to know how to spell it as a URL.
        if (std::optional<Url> url = Url::parse(text); url && !url->scheme().empty()) {
            inputs.push_back(*std::move(url));
        } else {
            inputs.push_back(Url::fromLocalPath(std::filesystem::path{text}));
        }
    }
    if (inputs.empty()) {
        return {};
    }

    const std::string jobId = remoteJobs_.start();

    ScanRequest request;
    request.inputs = std::move(inputs);
    request.atRow  = at ? static_cast<int>(*at) : -1;
    request.jobId  = jobId;
    pendingScans_.push_back(std::move(request));
    pumpScanQueue();

    return jobId;
}

// --- the selection's commands -----------------------------------------------

bool Session::savePlaylist(const std::filesystem::path& path, const std::vector<TrackId>* selection) {
    // What is written, decided before anything else. Either way the order is
    // the view's rather than the playlist's -- the sort the listener applied,
    // and only the rows the filter leaves. Sorting is display-only and never
    // reaches Playlist, so playlist_.entries() is still in the order the
    // tracks were added; a listener who sorts by album and saves wants the
    // file in album order.
    std::vector<PlaylistEntry> entries;
    if (selection != nullptr) {
        entries.reserve(selection->size());
        for (const TrackId id : *selection) {
            if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
                entries.push_back(*entry);
            }
        }
    } else {
        entries = view_.visibleEntries();
    }

    const std::string extension = pathToUtf8Generic(path.extension());
    PlaylistFormat    format    = PlaylistFormat::M3u;
    if (extension == ".pls") {
        format = PlaylistFormat::Pls;
    } else if (extension == ".xspf") {
        format = PlaylistFormat::Xspf;
    }

    // The queue is translated from ids to positions, and that is a real
    // conversion: Playlist::queue() holds TrackIds, which are opaque and
    // permanent; writePlaylist wants indices into the entries it is handed,
    // because that is what Cog's XML stores and what readPlaylist gives back.
    // Positions in the list being written, not rows of the playlist: a queued
    // track the filter hid, or that the selection left out, is not in that
    // list, so it is dropped rather than left naming some other row.
    std::unordered_map<TrackId, std::size_t> written;
    written.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        written.emplace(entries[i].id, i);
    }
    std::vector<std::size_t> queuePositions;
    for (const TrackId id : playlist_.queue()) {
        if (const auto found = written.find(id); found != written.end()) {
            queuePositions.push_back(found->second);
        }
    }

    // PlaylistFile writes text and the caller does the file I/O, which is what
    // keeps it testable without a filesystem. The destination goes in because
    // relative paths are written against it -- which is what makes a playlist
    // survive moving a music folder wholesale.
    const std::string text = writePlaylist(format, entries, queuePositions, Url::fromLocalPath(path));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        return false;
    }

    // A selection save says how many it wrote, because that is the number the
    // listener is checking. A whole-playlist save says so only when the filter
    // left tracks out: writing what is shown is the point of the ordering
    // above, but it is also the one way this command can quietly write fewer
    // tracks than the listener thinks it has.
    const std::size_t total = playlist_.size();
    if (selection != nullptr) {
        setStatus(fmt(trn("Playlist saved: %zu selected track.", "Playlist saved: %zu selected tracks.",
                          entries.size()),
                      entries.size()));
    } else if (entries.size() < total) {
        setStatus(trf("Playlist saved: %zu of %zu tracks, the rest hidden by the filter.",
                      entries.size(), total));
    } else {
        setStatus(tr("Playlist saved."));
    }
    return true;
}

void Session::trashTracks(const std::vector<TrackId>& ids) {
    std::vector<TrackId> trashed;
    trashed.reserve(ids.size());
    std::size_t failed = 0;
    for (const TrackId id : ids) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        const std::optional<std::filesystem::path> path = entry->url.localPath();
        if (!path) {
            continue;
        }
        if (platform::moveToTrash(*path)) {
            trashed.push_back(id);
        } else {
            // A read-only volume, a network share with no trash, or a file
            // already gone. The row stays, because the file did.
            ++failed;
        }
    }

    commands_.removeTrashed(std::move(trashed));

    if (failed > 0) {
        setStatus(fmt(trn("%zu file could not be moved to the trash.",
                          "%zu files could not be moved to the trash.", failed),
                      failed));
    } else {
        setStatus(statusSummary());
    }
}

void Session::revealInFileManager(const std::vector<TrackId>& ids) {
    // The first one, as Cog does (PlaylistController.m:1849). Revealing a
    // multiple selection means a file manager window per folder, which is not
    // what anybody means by "show me where this is".
    for (const TrackId id : ids) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (const std::optional<std::filesystem::path> path = entry->url.localPath()) {
            if (!platform::revealInFileManager(*path)) {
                setStatus(tr("Could not show the file."));
            }
            return;
        }
    }
}

void Session::resetPlayCount(const std::vector<TrackId>& ids) {
    const std::size_t count = commands_.resetPlayCount(ids);
    tracksUpdated.publish();
    setStatus(fmt(trn("Play count reset for %zu track.", "Play count reset for %zu tracks.",
                      count),
                  count));
}

void Session::removeRating(const std::vector<TrackId>& ids) {
    if (!library_) {
        return;
    }
    const std::size_t count = commands_.removeRating(ids);
    // Said in the status line because there is nowhere else it could show:
    // XPCog has no rating column and no star control, so a rating is something
    // a Cog library brought with it and this is the command that clears it.
    // Silence here would be indistinguishable from the command doing nothing.
    setStatus(fmt(trn("Rating removed from %zu track.", "Rating removed from %zu tracks.",
                      count),
                  count));
}

// --- settings -----------------------------------------------------------------

void Session::setVolume(double gain) {
    playback_->setVolume(gain);
    // MPRIS publishes the volume as a property a desktop environment both reads
    // and writes, so the value has to be pushed or the panel's slider sits
    // wherever it last put it while the audio does something else.
    if (media_) {
        media_->setVolume(static_cast<float>(gain));
    }
}

void Session::settingChanged(std::string_view key) {
    const Effect effect = effectOf(key).effect;
    switch (effect) {
        case Effect::ReloadDsp:
        case Effect::EqualizerCurve:
        case Effect::RefreshSpeed:
            // The engine holds the DSP chain; the frontend's sliders and the
            // speed popup redraw from effectApplied.
            playback_->reloadDsp();
            break;

        case Effect::GenreEqualizer:
            // Turning genre tracking on applies the playing track's genre at
            // once. Cog's -toggleTracking: does the same, and the reason is that
            // the alternative -- waiting for the next track -- makes the
            // checkbox look like it did nothing. Turning it *off* deliberately
            // leaves the curve where it is: the last preset it chose is as good
            // a starting point as any, and silently flipping back to some
            // remembered curve would be a second surprise.
            if (settings_.GraphicEqTrackGenre()) {
                // Past the memo deliberately: the playing track has almost
                // certainly been matched already, and the whole point of the
                // toggle is to act on it now.
                lastGenreTrack_ = kInvalidTrackId;
                lastGenre_.clear();
                applyGenreEqualizer(playlist_.find(currentTrack_));
            }
            break;

        case Effect::ReopenOutput:
            // The device is read when the engine opens it, which is when a
            // track starts. Moving what is already playing is what
            // reopenOutput() is for.
            playback_->reopenOutput();
            break;

        case Effect::PlaylistMode:
            // The playlist's own three, which it holds as state rather than
            // reading when it needs them. The constructor seeds all three and,
            // until this branch existed, nothing ever seeded them again -- so
            // "Stop after every track" in Preferences did nothing at all until
            // the next launch, and silently.
            playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
            playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
            playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());
            break;

        case Effect::Volume: {
            // Volume is seeded into the engine by PlaybackController's
            // constructor and into the slider by the frontend, and both keep it
            // from then on -- so the row in Advanced moved a number nothing
            // read again. The slider has to be moved too, or the interface
            // disagrees with what is coming out of the speakers.
            const double gain = settings_.Volume();
            setVolume(gain);
            volumeChanged.publish(gain);
            break;
        }

        case Effect::Scrobbler:
            if (scrobbler_) {
                scrobbler_->setEnabled(settings_.EnableScrobbling());
            }
            if (listenBrainzScrobbler_) {
                listenBrainz_->setApiRoot(settings_.ListenBrainzUrl());
                listenBrainzScrobbler_->setEnabled(settings_.EnableListenBrainz());
            }
            break;

        case Effect::OnlineLyrics:
            // The root first, so a redraw that asks asks the server now named.
            // The switch and the redraw are the frontend's, from lyricsLookup().
            if (lyricsLookup_) {
                lyricsLookup_->setApiRoot(settings_.LrclibUrl());
            }
            break;

        case Effect::CrashReporter:
            // Immediately, in both directions, which is the half of Cog's
            // arrangement that is easy to leave out: its observer on
            // `sentryConsented` calls `[SentrySDK close]` the moment the box is
            // unticked (AppController.m:417-420), rather than waiting for a
            // relaunch. Anything else means unticking the box and still being
            // reported on for the rest of the session.
            if (settings_.SentryConsented()) {
                platform::startCrashReporting();
            } else {
                platform::stopCrashReporting();
            }
            settings_.sync();
            break;

        case Effect::RestartRemote:
            applyRemoteSettings();
            break;

        case Effect::WaveformSeekBar:
            // On: the audible track's shape, asked for now rather than at the
            // next track. Off: whatever is being analysed is not wanted.
            if (settings_.WaveformSeekBar()) {
                requestWaveform(currentTrack_);
            } else {
                waveforms_->cancel();
                waveformUpdated.publish(nullptr);
            }
            break;

        case Effect::MiniFloating:
        case Effect::RefreshSpectrum:
        case Effect::RefreshScope:
        case Effect::RefreshPanels:
        case Effect::None:
        case Effect::Internal:
            // Nothing below the window changes. The frontend's arm follows.
            break;
    }

    effectApplied.publish(effect, std::string(key));
}

// --- scrobbling ---------------------------------------------------------------

void Session::wireScrobbling() {
    lastFm_ = std::make_unique<LastFmAccount>();

    // Beside the library rather than beside the settings: it is a queue of
    // pending work, not a preference, and it can be deleted without losing
    // anything the listener chose.
    const std::filesystem::path queue = dataDirectory_ / "scrobble-queue.json";

    scrobbler_ = std::make_unique<Scrobbler>(lastFm_->client(), queue);
    scrobbler_->setSession(lastFm_->load());
    scrobbler_->setEnabled(settings_.EnableScrobbling());

    // Last.fm rejected the stored key. Forget it here as well as in the
    // scrobbler, or the next launch would load the same dead key and fail again
    // -- and say so, because the listener has to re-authorise and nothing else
    // in the interface would ever mention it.
    scrobbler_->onSessionInvalidated([this] {
        dispatch_([this] {
            lastFm_->forget();
            setStatus(tr("Last.fm access was withdrawn. Reconnect in Preferences."));
        });
    });

    // ListenBrainz beside it, with a queue of its own: the two services take
    // and refuse plays independently, and one queue would have to remember
    // which of them each entry was still owed to.
    listenBrainz_          = std::make_unique<ListenBrainzAccount>(settings_.ListenBrainzUrl());
    listenBrainzScrobbler_ = std::make_unique<Scrobbler>(
        listenBrainz_->client(), queue.parent_path() / "listenbrainz-queue.json");
    listenBrainzScrobbler_->setSession(listenBrainz_->load());
    listenBrainzScrobbler_->setEnabled(settings_.EnableListenBrainz());
    listenBrainzScrobbler_->onSessionInvalidated([this] {
        dispatch_([this] {
            listenBrainz_->forget();
            setStatus(tr("ListenBrainz rejected the token. Reconnect in Preferences."));
        });
    });

    // Sixty seconds, which is Cog's interval for this (OutputNode.m:135-138).
    // Counted for every listener, whether or not they scrobble: this is
    // XPCog's own library.
    monitor_.onPlayCountReached([this] {
        if (!library_ || currentTrack_ == kInvalidTrackId) {
            return;
        }
        const PlaylistEntry* entry = playlist_.find(currentTrack_);
        if (entry == nullptr) {
            return;
        }
        if (!library_->recordPlay(*entry, unixNow())) {
            return;
        }

        // Mirrored back onto the entry, which is the copy the Info pane reads
        // and the copy the playlist persists. Writing only the table left the
        // count on screen frozen at whatever it was when the playlist was last
        // loaded, and let the next whole-playlist save write that stale number
        // back over the row.
        const TrackId id    = currentTrack_;
        std::int64_t  count = entry->playCount + 1;
        if (const auto record = library_->playCount(entry->artist, entry->album, entry->title())) {
            count = record->count;
        }

        playlist_.update(id, [count](PlaylistEntry& target) { target.playCount = count; });
        if (const PlaylistEntry* updated = playlist_.find(id)) {
            static_cast<void>(library_->saveEntry(*updated));
        }
        tracksUpdated.publish();
    });

    // Half the track or four minutes, whichever came first. The captured
    // pendingScrobble_ is submitted rather than the current entry, for the
    // reason given where it is declared.
    monitor_.onScrobbleReached([this] {
        if (pendingScrobble_.artist.empty()) {
            return;
        }
        if (scrobbler_) {
            scrobbler_->submit(pendingScrobble_);
        }
        if (listenBrainzScrobbler_) {
            listenBrainzScrobbler_->submit(pendingScrobble_);
        }
    });
}

void Session::wireLyrics() {
    // A build without libcurl has no way to ask, and the pane is then the
    // file's lyrics and nothing else -- which is what it was before there was a
    // lookup, and what the General pane's greyed row says.
    if (!httpClientAvailable()) {
        return;
    }
    lyricsHttp_ = makeCurlHttpClient();
    if (!lyricsHttp_) {
        return;
    }
    // The library remembers the answers, when there is one. Without it -- the
    // database would not open -- the lookup still works, for the session.
    if (library_) {
        lyricsStore_ = std::make_unique<LibraryLyricsStore>(*library_);
    }
    lyricsLookup_ = std::make_unique<LyricsLookup>(*lyricsHttp_, dispatch_, lyricsStore_.get(),
                                                   settings_.LrclibUrl());
}

void Session::beginScrobbleTrack(TrackId id, bool looping) {
    const PlaylistEntry* entry = (id == kInvalidTrackId) ? nullptr : playlist_.find(id);
    if (entry == nullptr) {
        monitor_.clear();
        pendingScrobble_ = ScrobbleTrack{};
        return;
    }

    pendingScrobble_             = ScrobbleTrack{};
    pendingScrobble_.title       = entry->title();
    pendingScrobble_.artist      = entry->artist.str();
    pendingScrobble_.albumArtist = entry->albumArtist.str();
    pendingScrobble_.album       = entry->album.str();
    pendingScrobble_.trackNumber = entry->track;
    pendingScrobble_.duration    = entry->duration();
    // When it *started*, not when the threshold is reached: Last.fm builds the
    // listening history from this, so a scrobble queued through an outage and
    // sent an hour later still lands in the right place.
    pendingScrobble_.startedAt = unixNow();

    if (looping) {
        monitor_.repeatTrack(playback_->playedSeconds(), entry->duration());
    } else {
        monitor_.beginTrack(playback_->playedSeconds(), entry->duration());
    }

    if (scrobbler_) {
        scrobbler_->nowPlaying(pendingScrobble_);
    }
    if (listenBrainzScrobbler_) {
        listenBrainzScrobbler_->nowPlaying(pendingScrobble_);
    }
}

// --- playback -----------------------------------------------------------------

void Session::onCurrentTrackChanged(TrackId id, ListenChange change) {
    // Repeat-one asks the playlist what follows a track and is told the same
    // track, so the seam announces an entry that never stopped playing. That is
    // one listen, not one a minute: without this a track left looping overnight
    // would report several hundred plays of itself. Cog counts each lap
    // (OutputNode.m resets its accumulator per stream); this deliberately does
    // not -- see docs/PORTING.md.
    const bool looping =
        change == ListenChange::Announced && id != kInvalidTrackId && id == currentTrack_;

    currentTrack_ = id;
    view_.setCurrentTrack(id);

    // Before anything that can fail below it: this is the point the seam reached
    // the speaker, and it is the only moment at which "a new track started" is
    // true exactly once.
    beginScrobbleTrack(id, looping);

    // Cog moves the selection as each next entry is *chosen*, inside
    // -getNextEntry: (PlaylistController.m:1448-1522, six call sites). Done
    // here instead, as the track actually becomes current, which lands on the
    // same rows in the same order and needs one call site rather than six.
    if (settings_.SelectionFollowsPlayback() && id != kInvalidTrackId) {
        revealRequested.publish(id);
    }

    // The summary back in the status line. "Connecting to X..." goes there when
    // the start is requested, and this is the moment it stopped being true.
    setStatus(statusSummary());

    const PlaylistEntry* entry = playlist_.find(id);
    publishNowPlaying(id);
    notifyTrack(entry);
    applyGenreEqualizer(entry);
    if (!looping) {
        requestWaveform(id);
    }

    trackChanged.publish(id, entry, looping);
}

void Session::onPlaybackStateChanged(bool playing, bool paused) {
    // Recorded as it changes rather than at exit, which is Cog's vocabulary and
    // the safer moment: a player that crashed while stopped must not come back
    // playing, and a status written only on a tidy exit says nothing about the
    // session that did not have one.
    settings_.setLastPlaybackStatus(!playing ? 0 : (paused ? 2 : 1));

    if (taskbar_) {
        taskbar_->setPlaybackState(playing, paused);
    }

    if (!playing) {
        if (media_) {
            media_->clear();
        }
        mediaPosition_ = -1.0;
    } else {
        if (media_) {
            media_->setPlaybackState(playing, paused, playback_->position());
        }
        mediaPosition_ = playback_->position();
    }

    playbackStateChanged.publish(playing, paused);
}

void Session::onPositionChanged(double seconds, double duration) {
    // The engine's own clock, not `seconds`. This tick is what advances the
    // played-time accumulator, and the two numbers are deliberately different:
    // `seconds` is the playhead, which a seek moves, while playedSeconds() is
    // audio actually delivered to the device, which a seek does not. Feeding
    // the playhead in here would let seeking to the end of a track scrobble it.
    monitor_.advance(playback_->playedSeconds());

    // The OS extrapolates from the rate it was given, so pushing every tick
    // would be four rewrites a second for a display that is already counting
    // correctly on its own. A second's drift is the threshold worth correcting.
    if (std::abs(seconds - mediaPosition_) >= 1.0) {
        mediaPosition_ = seconds;
        if (media_) {
            media_->setPlaybackState(playback_->playing(), playback_->paused(), seconds);
        }
    }

    positionChanged.publish(seconds, duration);
}

void Session::publishNowPlaying(TrackId id) {
    mediaPosition_ = -1.0;
    if (!media_) {
        return;
    }

    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        media_->clear();
        return;
    }

    platform::NowPlayingInfo info;
    info.title    = entry->title();
    info.artist   = entry->artist;
    info.album    = entry->album;
    info.duration = entry->duration();
    info.position = playback_->position();

    // Artwork is content-addressed in the library rather than carried on the
    // entry, so this is the one place that can resolve it. Handed over as the
    // encoded bytes the file carried, not as a decoded image.
    if (library_ && !entry->artHash.empty()) {
        info.artwork = library_->artwork(entry->artHash);
    }

    media_->setNowPlaying(info);
}

void Session::notifyTrack(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        // Stopped, or the track failed. Forgetting what was announced is what
        // lets the same track announce itself again when it is played again.
        lastNotified_ = kInvalidTrackId;
        return;
    }
    if (entry->error || !settings_.NotificationsEnable()) {
        return;
    }

    // Once per track, not once per call, and the difference is not defensive.
    // onCurrentTrackChanged is a redraw-everything handler and is *meant* to
    // run more than once for one track: PlaybackController publishes when the
    // decoder opens the track and again when the gapless seam reaches the
    // speaker, and trackMetadataChanged calls it a third time whenever a stream
    // renames itself. Redrawing a title bar twice costs nothing. Announcing a
    // track twice is two notifications.
    if (entry->id == lastNotified_) {
        return;
    }
    lastNotified_ = entry->id;

    // Cog's text, from PlaybackEventController.m:172-186. "Now Playing" is the
    // title; the body is the track title, then artist and album on the line
    // below, joined only where both exist so that a file with neither does not
    // announce itself with a dangling dash.
    std::string subtitle;
    if (!entry->artist.empty() && !entry->album.empty()) {
        subtitle = entry->artist.str() + " - " + entry->album.str();
    } else if (!entry->artist.empty()) {
        subtitle = entry->artist;
    } else {
        subtitle = entry->album;
    }

    std::string body = entry->title();
    if (!subtitle.empty()) {
        body += "\n" + subtitle;
    }

    // The cover, as the bytes the file carried; the frontend decodes it for
    // whatever its notification wants. Shared rather than copied: the same
    // cover is wanted by the info panel and the now-playing display.
    std::shared_ptr<const std::vector<std::byte>> cover;
    if (settings_.NotificationsShowAlbumArt() && library_ && !entry->artHash.empty()) {
        cover = library_->sharedArtwork(entry->artHash);
        if (cover && cover->empty()) {
            cover.reset();
        }
    }

    announceTrack.publish(tr("Now Playing"), body, cover);
}

void Session::applyGenreEqualizer(const PlaylistEntry* entry) {
    if (!settings_.GraphicEqTrackGenre()) {
        return;
    }

    if (entry == nullptr) {
        // Stopped, or the track failed. Forgetting what was matched is what
        // lets the same track be matched again when it is played again.
        lastGenreTrack_ = kInvalidTrackId;
        lastGenre_.clear();
        return;
    }

    // Once per track and genre, not once per call -- see the members for why
    // this handler runs more than once for one track, and what an unguarded
    // second run would cost: 32 settings rewritten, undoing a slider the
    // listener moved between the decoder opening the track and the seam
    // reaching the speaker.
    if (entry->id == lastGenreTrack_ && entry->genre == lastGenre_) {
        return;
    }
    lastGenreTrack_ = entry->id;
    lastGenre_      = entry->genre;

    const EqualizerPresetLibrary& library = shippedEqualizerPresets();
    const int                     index   = library.matchGenre(entry->genre);
    const EqualizerPreset*        preset  = library.at(index);
    if (preset == nullptr) {
        // No library shipped, so there is no preset to choose. Leaving the
        // curve alone is the only sensible answer: the setting asked for a
        // genre's preset, not for the equaliser to be reset.
        return;
    }

    settings_.setGraphicEqPreset(index);
    applyEqualizerPreset(settings_, *preset);
    playback_->reloadDsp();
    // The frontend's sliders redraw from this, the same way they do when the
    // remote control picks a preset.
    effectApplied.publish(Effect::EqualizerCurve, "GraphicEQpreset");
}

void Session::restorePlayback() {
    // Cog's shape (AppController.m:266-292): if the last session was not
    // stopped, find the entry the library marked current and *select* it --
    // always -- and start it only if the listener asked for that. Selecting
    // either way is the part worth copying: coming back to a playlist with the
    // last thing you were listening to highlighted is useful even to someone
    // who does not want it playing the moment the window opens.
    const int last = settings_.LastPlaybackStatus();
    if (last == 0) {
        return;
    }

    const auto current = playlist_.current();
    if (!current) {
        return;
    }

    revealRequested.publish(*current);

    if (!settings_.ResumePlaybackOnStartup()) {
        return;
    }

    double position = 0.0;
    try {
        const std::string stored = settings_.rawValue("xpcog.playback.position");
        position                 = stored.empty() ? 0.0 : std::stod(stored);
    } catch (const std::exception&) {
        // A value that will not parse is a position we do not have, not a
        // reason to refuse to play. The top of the track is the honest fallback.
        position = 0.0;
    }

    // Queued rather than started here: start() runs from the frontend's
    // constructor, and starting playback before the window exists means the
    // first track change redraws widgets that are still being built.
    const TrackId id = *current;
    dispatch_([this, id, position, last] { playback_->resumeTrack(id, position, last == 2); });
}

// --- the waveform ---------------------------------------------------------------

void Session::requestWaveform(TrackId id) {
    // Cleared first, whatever follows: the bars must not keep showing the last
    // track's shape over this one.
    waveformUpdated.publish(nullptr);

    const PlaylistEntry* entry = playlist_.find(id);
    if (!settings_.WaveformSeekBar() || entry == nullptr) {
        waveforms_->cancel();
        return;
    }
    waveforms_->request(entry->url);
}

void Session::onWaveformUpdated(const Url&                                    url,
                                const std::shared_ptr<const WaveformSummary>& summary) {
    const std::optional<Url> current = currentTrackUrl();
    if (!current || current->toString() != url.toString()) {
        return;
    }

    waveformUpdated.publish(summary);

    // The bar being looked at is done; now the guess at the next one, so it is
    // whole when it starts. The guess is the playlist's and can be wrong -- the
    // queue may change, a shuffle may draw differently -- and a wrong one costs
    // a spare analysis that ends up in the cache anyway.
    if (summary && summary->complete()) {
        if (const std::optional<TrackId> next = playlist_.peekNextForPlayback()) {
            if (const PlaylistEntry* entry = playlist_.find(*next);
                entry != nullptr && entry->url.toString() != url.toString()) {
                waveforms_->prefetch(entry->url);
            }
        }
    }
}

std::optional<Url> Session::currentTrackUrl() const {
    const PlaylistEntry* entry = playlist_.find(currentTrack_);
    return entry != nullptr ? std::optional{entry->url} : std::nullopt;
}

// --- the remote control ---------------------------------------------------------

void Session::applyRemoteSettings() {
    // Stopped first whatever happens next. A port or address change is a
    // restart, and stop() releases every request waiting on this thread before
    // it joins, so this does not block for the gate's full timeout.
    if (remoteServer_) {
        remoteServer_->stop();
        remoteServer_.reset();
    }

    if (!settings_.RemoteEnable()) {
        return;
    }
    if (!remote::remoteServerAvailable()) {
        // A build without XPCOG_WITH_REST. The pane greys itself and says so;
        // this is the path where the setting was carried over from a build
        // that had one.
        return;
    }

    const std::string token = RemoteToken::ensure();
    if (token.empty()) {
        std::string problem;
        static_cast<void>(RemoteToken::storeAvailable(&problem));
        setStatus(problem.empty() ? tr("The remote control could not create an access token.")
                                  : problem);
        return;
    }

    if (!remoteControl_) {
        remoteControl_ = std::make_unique<AppPlayerControl>(
            *playback_, playlist_, view_, commands_, settings_, remoteJobs_, library_.get(),
            [this](std::vector<std::string> urls, std::optional<std::size_t> at) {
                return startRemoteScan(std::move(urls), at);
            });

        // The fourth publisher of settingChanged, beside the preferences dialog
        // and the two panels, wired to the same handler. Without it a remote
        // write would be stored and inert until the next launch.
        subscriptions_.push_back(remoteControl_->settingChanged.connect(
            [this](const std::string& key) { settingChanged(key); }));
    }

    remote::ServerConfig config;
    config.address                   = settings_.RemoteAddress();
    config.port                      = settings_.RemotePort();
    config.token                     = token;
    config.allowWrite                = settings_.RemoteAllowWrite();
    config.allowLoopbackWithoutToken = settings_.RemoteLoopbackNoToken();

    remoteServer_ =
        std::make_unique<remote::RemoteServer>(*remoteControl_, dispatch_, std::move(config));

    std::string error;
    if (!remoteServer_->start(&error)) {
        // The ordinary failure is a port already taken, and it has to be
        // visible: a switch that silently did nothing is worse than one that
        // says why.
        setStatus(trf("Remote control unavailable: %s", error));
        remoteServer_.reset();
        return;
    }

    setStatus(trf("Remote control listening on http://%s:%d", settings_.RemoteAddress(),
                  remoteServer_->boundPort()));
}

}  // namespace xpcog::app
