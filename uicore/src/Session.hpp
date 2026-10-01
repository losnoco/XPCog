// Everything the window needs that is not a window.
//
// MainFrame was the wx application's composition root as well as its window:
// it owned the playlist, the library, the playback controller, the scan queue,
// the scrobblers, the remote server and the desktop integration, and wired all
// of them to each other. That worked while there was one window. With two
// frontends it is the wiring itself that has to be shared, or the second one
// writes it again and the two drift -- and the ImGui experiment (docs/GTKPORT.md
// says where) did write it again, which is the evidence.
//
// So this is the object graph, assembled once, with the window taken out. What
// stays in a frontend is what draws: menus, panes, dialogs, the status bar's
// text field, the decision to ask before trashing. What lives here is what
// happens: a folder is scanned and its tracks inserted, a track becomes audible
// and is scrobbled, announced and reported to the desktop, a setting changes
// and the engine is told. The frontend learns about all of it through the
// signals below, on the interface's thread, and never has to know which of
// three objects a fact came from.
//
// Two lifetime rules, both inherited from MainFrame and both load-bearing.
// **Subscriptions are held**: xpcog::Signal hands back an RAII token and
// letting go of it disconnects, so every connection is kept in
// `subscriptions_`, declared last so it goes first. **Members are declared in
// dependency order**, because they are destroyed in reverse: the remote server
// goes before the controller its workers call into, the scan before the cache
// it reads, the view before the playlist it indexes. The destructor's comments
// name the two that cannot be left to ordering alone.

#pragma once

#include "AppCommands.hpp"
#include "AppPlayerControl.hpp"
#include "PlaybackController.hpp"
#include "RemoteJobs.hpp"
#include "SettingEffect.hpp"

#include "xpcog/core/Dispatcher.hpp"
#include "xpcog/core/PlayMonitor.hpp"
#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/audio/WaveformProvider.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/ScanTask.hpp"
#include "xpcog/core/lyrics/LibraryLyricsStore.hpp"
#include "xpcog/core/lyrics/LyricsLookup.hpp"
#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/remote/RemoteServer.hpp"
#include "xpcog/core/scrobble/Scrobbler.hpp"
#include "xpcog/platform/MediaIntegration.hpp"
#include "xpcog/platform/TaskbarIntegration.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::app {

class LastFmAccount;
class ListenBrainzAccount;

class Session {
public:
    /// What a test substitutes. A frontend passes none of it.
    struct Options {
        /// Where the library database and the scrobble queues live, instead
        /// of platform::libraryDatabasePath()'s directory. Empty for the
        /// platform's.
        std::string dataDirectory;
        /// Where the waveform cache lives, instead of platform::cacheDirectory().
        std::string cacheDirectory;
        /// The device the engine plays into; see PlaybackController::OutputFactory.
        PlaybackController::OutputFactory makeOutput;
    };

    /// Neither the registry nor the settings are owned: both outlive every
    /// window, and the application object holds them. `dispatch` runs a
    /// callable on the interface's thread from any thread, and is handed to
    /// everything below that needs one.
    Session(const PluginRegistry& registry, Settings& settings, Dispatcher dispatch,
            Options options = {});
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // --- lifecycle --------------------------------------------------------

    /// Creates the desktop integration -- the OS's now-playing card, the media
    /// keys, the taskbar button -- around the native window handle, and wires
    /// its requests to the transport.
    ///
    /// Separate from the constructor because both want a window that exists:
    /// SMTC binds to an HWND, and there is none until the frontend has made
    /// one. Call it once, before start(). On platforms whose integration wants
    /// no handle, pass nullptr.
    void attachDesktop(void* nativeWindow);

    /// Loads the saved playlist, puts the last session back, and starts the
    /// remote server. After attachDesktop() and after the frontend has
    /// subscribed to the signals below, since both are told things here.
    ///
    /// The remote server starts last, after the playlist is loaded: a client
    /// that connects the instant the port opens should find the player it is
    /// about to be told about, not an empty one.
    void start();

    /// Gives the playback controller its heartbeat. The frontend calls this
    /// every PlaybackController::kTickIntervalMs; calling it more often costs a
    /// branch.
    void tick();

    /// Writes the playlist and where playback had got to, and syncs the
    /// settings. The frontend writes its own keys -- geometry, layout -- before
    /// calling this, so one sync covers both. Safe to call more than once.
    void save();

    // --- the playlist -----------------------------------------------------

    /// Scans `urls` -- files, folders, playlists, cue sheets, streams -- and
    /// inserts what they expand to at `atRow`, or at the end when it is
    /// negative. Queued behind any scan already running; see ScanRequest.
    void addUrls(const std::vector<Url>& urls, int atRow = -1);

    /// Re-reads the tags of `ids` in place, keeping what the playlist knows and
    /// the files do not. Through the same queue, and for the same reason.
    void reloadTracks(const std::vector<TrackId>& ids);

    /// Abandons the running scan and everything queued behind it.
    void cancelScans();

    [[nodiscard]] bool scanning() const noexcept { return scan_ != nullptr; }

    /// Writes the rows on screen -- the view's order, the filter's survivors
    /// -- or, with `selection`, those tracks in that order, to `path` in the
    /// format its extension names (.m3u8, .pls, .xspf). Says what it wrote on
    /// `status`; answers false when the file could not be written, which the
    /// frontend says in a dialog. The picker is the frontend's.
    bool savePlaylist(const std::filesystem::path& path, const std::vector<TrackId>* selection);

    /// Moves the local files among `ids` to the trash and takes the rows out
    /// of the playlist, undoably. Whether to ask first is the frontend's
    /// question; this does not ask.
    void trashTracks(const std::vector<TrackId>& ids);

    /// Shows the first of `ids` that is a local file in the desktop's file
    /// manager.
    void revealInFileManager(const std::vector<TrackId>& ids);

    void resetPlayCount(const std::vector<TrackId>& ids);
    void removeRating(const std::vector<TrackId>& ids);

    // --- playback and settings ----------------------------------------------

    /// Sets the engine's gain and tells the desktop, which shows a volume of
    /// its own. From the frontend's slider; the desktop's own requests arrive
    /// through volumeChanged instead.
    void setVolume(double gain);

    /// A setting was written; do whatever that key actually changes.
    ///
    /// Routed through SettingEffect rather than a chain of comparisons, which
    /// is what that table was written to replace -- and it has a test
    /// asserting every key in Settings::all() is covered, so a setting added
    /// later cannot quietly do nothing. The arms that reach the engine, the
    /// playlist, the scrobblers, the remote server and the crash reporter are
    /// handled here; every arm is then published as effectApplied, and the
    /// frontend does what is left, which is redrawing a widget.
    void settingChanged(std::string_view key);

    /// "%zu tracks -- h:mm:ss", for the status line.
    [[nodiscard]] std::string statusSummary() const;

    /// The last sentence published on `status`, for a frontend that subscribed
    /// after the constructor said something -- a library that would not open.
    [[nodiscard]] const std::string& lastStatus() const noexcept { return lastStatus_; }

    /// Which entry is audible, or kInvalidTrackId.
    [[nodiscard]] TrackId currentTrack() const noexcept { return currentTrack_; }

    // --- the objects the widgets bind to -------------------------------------

    [[nodiscard]] const PluginRegistry& registry() const noexcept { return registry_; }
    [[nodiscard]] Settings&             settings() noexcept { return settings_; }
    [[nodiscard]] Playlist&             playlist() noexcept { return playlist_; }
    [[nodiscard]] const Playlist&       playlist() const noexcept { return playlist_; }
    [[nodiscard]] PlaylistView&         view() noexcept { return view_; }
    [[nodiscard]] UndoStack&            undo() noexcept { return undo_; }
    [[nodiscard]] AppCommands&          commands() noexcept { return commands_; }
    [[nodiscard]] PlaybackController&   playback() noexcept { return *playback_; }
    [[nodiscard]] const PlaybackController& playback() const noexcept { return *playback_; }
    /// Null when the database would not open. The player still plays.
    [[nodiscard]] Library*              library() noexcept { return library_.get(); }
    [[nodiscard]] WaveformProvider&     waveforms() noexcept { return *waveforms_; }
    [[nodiscard]] const Dispatcher&     dispatcher() const noexcept { return dispatch_; }

    /// The lookup the Lyrics pane asks, or null: in a build with no HTTP
    /// client, or while `enableLrclib` is off. Re-read after
    /// effectApplied(Effect::OnlineLyrics).
    [[nodiscard]] LyricsLookup* lyricsLookup() noexcept;

    [[nodiscard]] LastFmAccount*       lastFm() noexcept { return lastFm_.get(); }
    [[nodiscard]] Scrobbler*           scrobbler() noexcept { return scrobbler_.get(); }
    [[nodiscard]] ListenBrainzAccount* listenBrainz() noexcept { return listenBrainz_.get(); }
    [[nodiscard]] Scrobbler* listenBrainzScrobbler() noexcept {
        return listenBrainzScrobbler_.get();
    }

    // --- what the frontend is told ---------------------------------------------
    //
    // All on the interface's thread. Each is published after this object has
    // done its own part, so a handler may read any accessor above and see the
    // state the signal describes.

    /// A sentence for the status line. Already translated.
    Signal<std::string> status;

    /// A scan began, and its progress: `done` of `total`, where a total of
    /// zero means the expansion pass is still counting and a busy indicator is
    /// the truthful display. Then it ended, whether it finished or was
    /// cancelled.
    Signal<>         scanStarted;
    Signal<int, int> scanProgress;
    Signal<>         scanFinished;

    /// The audible track changed: to `entry`, or to nothing. `looping` is the
    /// same entry coming round again under repeat, which is one listen rather
    /// than a second. Published after the scrobble clock, the desktop's card,
    /// the notification and the genre equaliser have all been dealt with.
    Signal<TrackId, const PlaylistEntry*, bool> trackChanged;

    /// PlaybackController's, republished after the desktop and the settings
    /// have been told. Subscribe here rather than there so the two cannot be
    /// seen in different orders.
    Signal<bool, bool>     playbackStateChanged;
    Signal<double, double> positionChanged;

    /// A setting's effect was applied here; what is left is the widget's half.
    /// See settingChanged().
    Signal<Effect, std::string> effectApplied;

    /// Put this track on screen and select it: a resumed session's last track,
    /// or the one that just started while the selection follows playback.
    Signal<TrackId> revealRequested;

    /// The desktop asked -- MPRIS Raise, the media keys' Quit.
    Signal<> raiseRequested;
    Signal<> quitRequested;

    /// The gain changed from somewhere other than the frontend's own slider,
    /// which has to move to match: the desktop's volume control, or the raw
    /// row in Advanced.
    Signal<double> volumeChanged;

    /// A track started and the listener wants told: the title, the body, and
    /// the cover's bytes when there are any and the setting asks for them.
    /// Once per track, whatever the seam does; the guard is here.
    Signal<std::string, std::string, std::shared_ptr<const std::vector<std::byte>>>
        announceTrack;

    /// The audible track's waveform, complete or still filling in. Only for the
    /// track that is audible now; a prefetch's answer waits in the cache.
    Signal<std::shared_ptr<const WaveformSummary>> waveformUpdated;

    /// Entries changed under the frontend in a way its Info and Lyrics panes
    /// should redraw for: a play count ticked over, tags were re-read.
    Signal<> tracksUpdated;

private:
    /// One scan at a time: the PluginCache the scans share is not synchronised.
    /// Requests arriving while one runs wait their turn, which also keeps a
    /// burst of drops landing in the order they were dropped.
    struct ScanRequest {
        std::vector<Url> inputs;
        int              atRow = -1;

        /// A reload rather than an addition: the results are merged onto the
        /// entries that are already there instead of being inserted. Both go
        /// through the same queue because they contend for the same
        /// PluginCache, and a reload starting mid-scan is the race that queue
        /// exists to stop.
        bool reload = false;

        /// Set when the scan was started over the API, so its progress can be
        /// reported back through GET /jobs/{id}. Empty for a scan the window
        /// started, which has the progress bar instead.
        std::string jobId;
    };

    void pumpScanQueue();
    void addScannedEntries(std::vector<PlaylistEntry> entries, int atRow, bool cancelled);
    void applyReloadedEntries(std::vector<PlaylistEntry> entries);

    /// Begins a scan on behalf of the API and answers the job id to follow.
    [[nodiscard]] std::string startRemoteScan(std::vector<std::string>   urls,
                                              std::optional<std::size_t> at);

    /// Starts or stops the server to match the settings. At startup and
    /// whenever one of the `remote*` keys changes, so a port change takes
    /// effect without a relaunch.
    void applyRemoteSettings();

    /// Builds the two accounts, the two scrobblers and the play clock, and
    /// wires the clock's thresholds to the library and the scrobblers.
    void wireScrobbling();

    /// Builds the lookup behind the Lyrics pane, when this build can make an
    /// HTTP request at all.
    void wireLyrics();

    /// Starts the play clock for `id`, and tells both services it is playing.
    void beginScrobbleTrack(TrackId id, bool looping);

    /// Why the current track is being announced, which the two listening
    /// thresholds need and nothing else does.
    enum class ListenChange {
        /// The seam reached the speaker. The same id arriving twice running is
        /// the same entry looping -- repeat-one, or a playlist of one under
        /// repeat-all -- rather than a second listen.
        Announced,
        /// The entry did not change but the song did: a stream renamed itself.
        /// A new listen, on a row that was already current.
        NewSong,
    };
    void onCurrentTrackChanged(TrackId id, ListenChange change);
    void onPlaybackStateChanged(bool playing, bool paused);
    void onPositionChanged(double seconds, double duration);

    /// Tells the OS what is playing. Reads the artwork from the library, which
    /// is why it lives here and not in PlaybackController.
    void publishNowPlaying(TrackId id);

    /// Publishes announceTrack for `entry`, once per track, if the listener
    /// wants announcing. Cog's text, from PlaybackEventController.
    void notifyTrack(const PlaylistEntry* entry);

    /// Points the equaliser at the preset matching `entry`'s genre, when
    /// `GraphicEQtrackgenre` says to. Cog does this from -didBeginStream:.
    ///
    /// An untagged track matches nothing and Cog's fallback for matching
    /// nothing is Flat, so this rewrites the curve at every track boundary
    /// rather than only when it has something to say.
    void applyGenreEqualizer(const PlaylistEntry* entry);

    /// Puts the last session back: reveals the track that was current, and
    /// resumes it where it left off when resumePlaybackOnStartup says to.
    void restorePlayback();

    /// Asks the provider for `id`'s shape, when the setting is on. The
    /// prefetch of what probably follows is asked for when this one completes.
    void requestWaveform(TrackId id);
    void onWaveformUpdated(const Url& url, const std::shared_ptr<const WaveformSummary>& summary);
    [[nodiscard]] std::optional<Url> currentTrackUrl() const;

    void setStatus(std::string text);

    // Declaration order is destruction order in reverse, and it matters: the
    // controller borrows the playlist and the registry, the view borrows the
    // playlist, the commands borrow the undo stack, and the lookup borrows the
    // store which borrows the library.
    const PluginRegistry& registry_;
    Settings&             settings_;
    Dispatcher            dispatch_;

    /// Where the library and the scrobble queues live; see Options.
    std::filesystem::path dataDirectory_;

    Playlist                 playlist_;
    PluginCache              cache_;
    std::unique_ptr<Library> library_;

    std::unique_ptr<IHttpClient>        lyricsHttp_;
    std::unique_ptr<LibraryLyricsStore> lyricsStore_;
    std::unique_ptr<LyricsLookup>       lyricsLookup_;

    PlaylistView view_;
    UndoStack    undo_;
    AppCommands  commands_;

    std::unique_ptr<PlaybackController> playback_;

    // --- scrobbling ---------------------------------------------------------
    // Beside the library and the playback controller because it needs both:
    // what was played comes from one and where to record it from the other.

    /// How much of the audible track has actually been heard. Drives two
    /// thresholds, exactly as Cog's OutputNode does: the play count at sixty
    /// seconds and the scrobble at half the track or four minutes.
    PlayMonitor monitor_;

    /// The play to submit when the threshold is crossed, captured when the
    /// track became audible rather than read back at submission time -- by
    /// then the entry may have been edited, or removed from the playlist
    /// entirely, and the play still happened.
    ScrobbleTrack pendingScrobble_;

    std::unique_ptr<LastFmAccount> lastFm_;
    std::unique_ptr<Scrobbler>     scrobbler_;

    /// The same again for ListenBrainz: its own account, its own queue. One
    /// play goes to both, and each service accepts or refuses it on its own.
    std::unique_ptr<ListenBrainzAccount> listenBrainz_;
    std::unique_ptr<Scrobbler>           listenBrainzScrobbler_;

    /// The OS's Now Playing entry and media keys, and the taskbar button.
    /// Null until attachDesktop(); every use is guarded.
    std::unique_ptr<platform::MediaIntegration>   media_;
    std::unique_ptr<platform::TaskbarIntegration> taskbar_;
    /// The position last pushed to the OS. It extrapolates from the rate, so
    /// pushing every transport tick would be four rewrites a second for a
    /// display that is already counting correctly on its own.
    double mediaPosition_ = -1.0;

    std::unique_ptr<ScanTask>         scan_;
    /// The seek bar's waveforms. Owned like the scan and for the same reason:
    /// it borrows the registry, so the destructor lets it go first.
    std::unique_ptr<WaveformProvider> waveforms_;
    /// The job id of the scan now running, if it came from the API.
    std::string              scanJobId_;
    std::vector<ScanRequest> pendingScans_;

    TrackId currentTrack_ = kInvalidTrackId;

    /// The track the last notification was about, so one track produces one.
    /// kInvalidTrackId while nothing is playing, which is what lets the same
    /// track announce itself again the next time it is started.
    TrackId lastNotified_ = kInvalidTrackId;

    /// What genre tracking last acted on, so one track produces one preset
    /// change. Paired with the genre because metadata can arrive after
    /// playback starts: a track whose genre was empty at the first call and
    /// filled in by the second should be matched again.
    TrackId     lastGenreTrack_ = kInvalidTrackId;
    std::string lastGenre_;

    std::string lastStatus_;
    bool        saved_ = false;

    // --- the REST remote control ---------------------------------------------
    // Declared after playback_, commands_ and library_, and that is a contract
    // rather than tidiness: the server -- whose stop() joins its listener and
    // releases every request waiting on this thread -- goes first, and nothing
    // it holds a reference to is gone while a worker might still be inside
    // handle().
    RemoteJobs                            remoteJobs_;
    std::unique_ptr<AppPlayerControl>     remoteControl_;
    std::unique_ptr<remote::RemoteServer> remoteServer_;

    /// Declared last; see the class comment.
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::app
