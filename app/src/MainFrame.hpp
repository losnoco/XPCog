// The player window.
//
// The wxWidgets counterpart of the Qt MainWindow, and it keeps that file's one
// useful convention: every connection this window makes lives in wireUp(). That
// was the answer to Cog's 190 bindings scattered across a dozen XIBs -- when the
// question is "what updates when the track changes", there is exactly one place
// to look -- and it is worth as much here.
//
// Two lifetime rules, both of which the toolkit used to enforce and no longer
// does:
//
// **Subscriptions are held.** Qt disconnected automatically when either end was
// destroyed. xpcog::Signal hands back an RAII token instead, and letting go of it
// is what disconnects, so every connection is kept in `subscriptions_` -- which
// is declared last, so it is destroyed first, before the objects whose signals it
// holds. A handler firing into a half-destroyed window is the failure that
// ordering exists to make impossible.
//
// **Commands are integers.** wx has no QAction, so a command is an id that a menu
// item, a toolbar button and an accelerator all post. Enabled state, checked
// state and labels come from EVT_UPDATE_UI rather than from a shared object,
// which means the menu bar, the playlist's context menu and the tray menu stay in
// step without anyone remembering to update three of them -- and Undo relabels
// itself from the stack rather than needing a refresh call.

#pragma once

#include "PlaybackController.hpp"
#include "StatusPresence.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/PlayMonitor.hpp"
#include "xpcog/core/library/CogImport.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/scrobble/Scrobbler.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/ScanTask.hpp"
#include "xpcog/platform/MediaIntegration.hpp"
#include "xpcog/platform/TaskbarIntegration.hpp"

#include <wx/aui/aui.h>
#include <wx/frame.h>

#include <functional>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <vector>

class wxDataViewCtrl;
class wxDataViewItem;
class wxGauge;
class wxBitmapButton;
class wxPanel;
class wxSearchCtrl;
class wxSlider;
class wxSplitterWindow;
class wxStaticText;
class wxToolBar;

namespace xpcog::app {

class LastFmAccount;

class EqualizerPanel;
class FileTree;
class InfoPanel;
class LyricsPanel;
class MiniFrame;
class PlaylistDataModel;
class Sc55Panel;
class SeekBar;
class SpectrumPanel;
class SpeedPanel;
enum class PreferencesPane;

using Dispatcher = std::function<void(std::function<void()>)>;

class MainFrame : public wxFrame {
public:
    MainFrame(const PluginRegistry& registry, Settings& settings, Dispatcher dispatch);
    ~MainFrame() override;

    /// Adds files, folders, playlists or cue sheets. Public so the command line
    /// and the OS's open-document event reach the same code path the menu does.
    void openUrls(const std::vector<Url>& urls);

private:
    void buildUi();
    /// The frame's toolbar: the transport buttons and the pane toggles.
    void buildToolBar();
    /// The strip under it: the seek bar, the clock, the volume and the filter.
    void buildControls(wxWindow* parent);

    /// Whether every tool's glyph has to be rendered again, or only the one
    /// that moves with the transport.
    ///
    /// `Yes` is for a system palette change, where each glyph is stroked in a
    /// colour that has just stopped being right. It is not the default because
    /// wxMSW's SetToolNormalBitmap() calls Realize() internally -- re-applying
    /// ten unchanged bitmaps is a full toolbar rebuild, and a track that starts,
    /// pauses and ends asks for one at each step.
    enum class Restroke { No, Yes };

    /// Re-strokes the toolbar's glyphs, and points Play/Pause at whichever of
    /// the two the current state calls for.
    ///
    /// Both jobs in one place because they are the same job: the bitmap has to
    /// be chosen from state *and* re-stroked when the system appearance
    /// changes, and two functions doing half each is how the Qt build ended up
    /// with an icon refresh that put "play" back over a running track. Play/Pause
    /// is settled after the loop for that reason, whichever way it was called.
    void refreshTransportIcons(Restroke restroke = Restroke::No);

    /// Every connection this window owns, in one place.
    void wireUp();
    void bindCommands();
    void bindUpdateUi();

    void openFiles();
    void openFolder();
    void openUrl();
    /// Writes a playlist file. `selectionOnly` is Cog's "Save Selection As
    /// Playlist...", which is the same command over fewer rows -- so it is a
    /// parameter rather than a second function that would have to be kept in
    /// step with this one's queue translation and its filter warning.
    void savePlaylistAs(bool selectionOnly);
    void showPreferences();
    /// Opens on a named pane. The Pitch & Tempo dock pane's Settings button is
    /// the one caller; everything else opens on Playlist as before.
    void showPreferences(std::optional<PreferencesPane> pane);
    void showAbout();

    /// Asks, once ever, whether XPCog may send crash reports.
    ///
    /// Cog's prompt and Cog's promise (Window/MainWindow.m:19-38): shown as the
    /// window appears on the first launch that has one, and never again --
    /// whatever the answer was, including no answer at all. Preferences is where
    /// it is changed afterwards, which is what the prompt says.
    ///
    /// Here rather than in XPCogApp because it is modal and needs a window to be
    /// modal to. Silently does nothing in a build with no reporter: consent to
    /// something that cannot happen is not worth interrupting anyone for.
    void askCrashReportingConsent();

    /// A setting changed and something has to be told. Shared by the
    /// preferences dialog and the equaliser panel, which both publish the same
    /// keys -- and the equaliser's is the case that must not be forgotten,
    /// since a band that does not reach the engine is a slider that does
    /// nothing.
    void onSettingChanged(const std::string& key);

    /// Switches between the full window and the mini player. A mode, as in
    /// Cog: one is shown and the other hidden, never both.
    void setMiniMode(bool mini);

    /// Which track the Info and Lyrics panes should be describing.
    ///
    /// One function because the two panes must never disagree -- they sit next
    /// to each other, and an Info pane showing one track beside a Lyrics pane
    /// showing another is a bug you cannot look away from. `panelFollowMode`
    /// chooses the rule, and its default is Cog's: the playlist selection when
    /// there is one, the playing track otherwise. See settings.def for why the
    /// alternative exists and why there are two modes and not three.
    [[nodiscard]] TrackId panelTrackId() const;

    /// Redraws the info panel. Cheap to call from anywhere, because it returns
    /// immediately while the panel is hidden -- which is most of the time, and
    /// matters because metadata arriving during a scan would otherwise redraw
    /// twenty fields per file.
    void refreshInfo();

    /// Announces a track as it starts, if the listener wants announcing.
    ///
    /// Cog's, from PlaybackEventController -performPlaybackDidBeginActions:
    /// (:148-200), including the shape of the text and the decision not to
    /// suppress it while the window is in front -- Cog does not check, and a
    /// player whose notification depends on where the focus is is a player whose
    /// notifications look unreliable.
    void notifyTrack(const PlaylistEntry* entry);

    /// Points the equaliser at the preset matching `entry`'s genre, when
    /// `GraphicEQtrackgenre` says to. Cog does this from -didBeginStream:, as
    /// each track actually starts.
    ///
    /// Here rather than in the panel because it is the frame that learns a track
    /// began, and rather than in core because choosing a preset for a genre is a
    /// policy about the library the interface presents -- core knows how to match
    /// a genre and stops there.
    ///
    /// Worth knowing before turning it on: an untagged track matches nothing and
    /// Cog's fallback for matching nothing is Flat, so this rewrites the curve at
    /// every track boundary rather than only when it has something to say.
    void applyGenreEqualizer(const PlaylistEntry* entry);

    /// Puts the last session back: selects the track that was current, and
    /// starts it where it left off when resumePlaybackOnStartup says to.
    /// Does nothing when the last session ended stopped.
    void restorePlayback();

    /// The track the last notification was about, so one track produces one.
    /// kInvalidTrackId while nothing is playing, which is what lets the same
    /// track announce itself again the next time it is started.
    TrackId lastNotified_ = kInvalidTrackId;

    /// What genre tracking last acted on, so one track produces one preset
    /// change.
    ///
    /// The same guard notifications need, and for a sharper reason.
    /// onCurrentTrackChanged is a redraw-everything handler that runs two or
    /// three times per track by design, and each extra run here would rewrite 32
    /// settings -- which is not merely wasteful: it would undo a slider the
    /// listener moved between the decoder opening the track and the gapless seam
    /// reaching the speaker.
    ///
    /// Paired with the genre rather than keyed on the track alone, because
    /// metadata can arrive after playback starts. A track whose genre was empty
    /// at the first call and filled in by the second should be matched again,
    /// and the pair is what tells that apart from the same call arriving twice.
    TrackId     lastGenreTrack_ = kInvalidTrackId;
    std::string lastGenre_;

    /// Redraws the lyrics pane, on the same rule and with the same guard.
    ///
    /// Separate from refreshInfo() rather than folded into it because the two
    /// panes are shown independently: the common case is one of them open, and
    /// a single function would do the hidden one's work anyway. They are called
    /// from the same three places.
    void refreshLyrics();

    /// Shows or hides one of the dockable panes.
    void togglePane(wxWindow* pane, bool show);

    /// Shows or hides the folder browser, which is the splitter's left pane
    /// rather than a dockable one. Hidden on a first launch; see buildUi().
    void showFileTree(bool show);

    /// Whether a pane is currently on screen -- which is not the same as
    /// wxWindow::IsShown(): a floating pane's window is shown while the pane
    /// itself may be closed, and a docked one in a hidden notebook tab is the
    /// reverse. The manager is the authority.
    [[nodiscard]] bool paneShown(wxWindow* pane) const;

    /// Docks every floating pane, and reports whether any was.
    ///
    /// The escape hatch for Wayland. A pane is docked by dragging it onto the
    /// frame, and a Wayland client cannot position its own surfaces -- so the
    /// drag that would re-dock a torn-off pane never reaches wxAUI, and a pane
    /// floated there stays floated for good. Each pane remembers the dock it
    /// came from, so this puts them back where they were rather than at a
    /// default.
    bool dockFloatingPanes();

    /// Whether any pane is torn off. Drives the menu item's enabled state.
    [[nodiscard]] bool anyPaneFloating();

    void addUrls(const std::vector<Url>& urls, int atRow = -1);

    /// Starts the next queued scan, if any and if none is running.
    void pumpScanQueue();

    /// Reads a Cog library and adds it to this playlist. The picker, the scan
    /// and the summary; the reading and the conversion are core's.
    void importFromCog();
    void addScannedEntries(std::vector<PlaylistEntry> entries, int atRow, bool cancelled);

    void onPositionChanged(double seconds, double duration);
    void onCurrentTrackChanged(TrackId id);
    void onPlaybackStateChanged(bool playing, bool paused);

    void removeSelected();
    void enqueueSelected();
    void activateRow(unsigned int row);

    // --- the playlist's context menu -------------------------------------
    //
    // Cog's ContextualMenu, one function per item. Every one of them acts on the
    // selection and does nothing when there is none, which is also what their
    // EVT_UPDATE_UI handlers say -- the guard is repeated because a command can
    // arrive from an accelerator that no menu ever disabled.

    /// Pops the menu up over the row that was right-clicked.
    ///
    /// Selects that row first when it was not already selected, and leaves a
    /// multiple selection alone when it was. That is Cog's rule
    /// (PlaylistView.m:274) and it is the one that matches what people expect:
    /// right-clicking one of five selected rows acts on the five, and
    /// right-clicking a sixth acts on the sixth.
    void showPlaylistMenu(const wxDataViewItem& item);

    /// Queues the selection, or takes it out of the queue.
    ///
    /// Per entry, as Cog's -toggleQueuedForEntries: is: a mixed selection flips
    /// each row rather than deciding one way for all of them.
    void toggleQueuedSelected();

    /// Flips `stopAfter` on each selected entry, so playback stops when that
    /// track ends. Cleared for free when the track is left -- see
    /// Playlist::moveCurrent().
    void toggleStopAfterSelected();

    /// Puts the selected track's artist or album in the filter box.
    ///
    /// Cog opens its Spotlight window here; the filter is what replaced that
    /// window, so this fills it in. Reads the first selected row, as Cog does --
    /// there is one search field and it can hold one answer.
    void searchForSelected(bool byAlbum);

    /// Re-reads the tags of the selected tracks, in place.
    void reloadSelectedInfo();

    /// Merges a reload's results back onto the entries they came from.
    ///
    /// By URL, never by position: the scan drops what it cannot open and expands
    /// a container into several, so the two sequences are different lengths --
    /// the same rule the Cog import follows and for the same reason.
    void applyReloadedEntries(std::vector<PlaylistEntry> entries);

    void resetPlayCountSelected();
    void removeRatingSelected();

    /// Shows the first selected track in the desktop's file manager.
    void revealSelected();

    /// Moves the selected files to the trash and takes them out of the playlist.
    /// Asks first, once, unless the listener has said not to.
    void trashSelected();

    /// The selection in the order it is displayed, which is not the order
    /// wxDataViewCtrl reports it in.
    [[nodiscard]] std::vector<TrackId> selectedTracksInOrder() const;

    /// The local files among the selection. Empty for a selection of streams,
    /// which is what disables the two commands that need a path.
    [[nodiscard]] std::vector<std::filesystem::path> selectedPaths() const;

    /// Points the header arrow at whatever PlaylistView is actually sorted by,
    /// and takes it away when that is nothing. The control has its own idea,
    /// which is two-state and therefore cannot express the third click.
    void showSortIndicator();

    [[nodiscard]] std::vector<TrackId> selectedTracks() const;

    /// Tells the OS what is playing. Reads the artwork from the library, which
    /// is why it lives here and not in PlaybackController.
    void publishNowPlaying(TrackId id);

    /// Saves everything that has to outlive the session: window geometry, the
    /// splitter, the file tree's root, the playlist, and the settings store.
    void persistState();
    void restoreState();

    /// Remembers the window's un-maximised rectangle.
    ///
    /// Tracked as it changes rather than read at save time, because a
    /// maximised window reports the maximised rectangle and there is no way
    /// to ask wx for the one it would restore to. Saving that would give a
    /// window that un-maximises to full screen -- which looks like the
    /// setting not working at all.
    void rememberGeometry();

    [[nodiscard]] wxString statusSummary() const;

    void setStatusText(const wxString& text);

    /// Re-labels every wxAUI pane from the catalogue.
    ///
    /// Not merely a tidy-up of buildUi(). A saved perspective stores each pane's
    /// *caption* alongside its position -- wxAuiManager::SavePaneInfo writes it
    /// and LoadPaneInfo assigns it straight back -- so a layout saved by an
    /// English session puts "Spectrum" back over the pane after the language has
    /// been changed to Spanish, and it stays that way for good because the next
    /// save records what was restored. Calling this after LoadPerspective is
    /// what stops the layout from being a second, silent store of interface
    /// text.
    void applyPaneCaptions();

    const PluginRegistry& registry_;
    Settings&             settings_;
    Dispatcher            dispatch_;

    Playlist                 playlist_;
    PluginCache              cache_;
    std::unique_ptr<Library> library_;
    PlaylistView             view_;
    UndoStack                undo_;

    std::unique_ptr<PlaybackController> playback_;

    // --- scrobbling -----------------------------------------------------
    // Owned here, beside the library and the playback controller, because it
    // needs both: what was played comes from one and where to record it from the
    // other. Cog puts the equivalent in a singleton reached from anywhere; this
    // is the same objects with the ownership visible.

    /// How much of the audible track has actually been heard. Drives two
    /// thresholds, exactly as Cog's OutputNode does: the play count at sixty
    /// seconds and the scrobble at half the track or four minutes.
    PlayMonitor monitor_;

    /// The play to submit when the threshold is crossed, captured when the track
    /// became audible rather than read back at submission time -- by then the
    /// entry may have been edited, or removed from the playlist entirely, and
    /// the play still happened.
    ScrobbleTrack pendingScrobble_;

    std::unique_ptr<LastFmAccount> lastFm_;
    std::unique_ptr<Scrobbler>     scrobbler_;

    /// Wires the monitor's two thresholds to the library and the scrobbler.
    void wireScrobbling();

    /// Starts the monitor for `id`, and announces it as now playing.
    void beginScrobbleTrack(TrackId id);

    /// The docking manager. Declared before the panes it manages so it is
    /// destroyed after them -- though UnInit() in the destructor is what
    /// actually makes teardown safe, and is not optional.
    wxAuiManager auiManager_;

    /// The last rectangle the window had while neither maximised nor
    /// minimised. See rememberGeometry().
    wxRect normalRect_;

    /// The window wxAuiManager actually manages, which is not the frame.
    ///
    /// See buildUi() for why the transport strip sits above it rather than in
    /// it. Everything dockable is a child of this.
    wxPanel* dockHost_ = nullptr;

    wxSplitterWindow* splitter_ = nullptr;
    FileTree*         tree_     = nullptr;
    /// How wide the folder browser was when it was last open, so closing and
    /// reopening it does not reset the width. See showFileTree().
    int fileTreeSash_ = 0;
    wxDataViewCtrl*   list_     = nullptr;
    /// Reference-counted by the control, so this is a borrowed pointer and must
    /// not be deleted here.
    PlaylistDataModel* model_ = nullptr;

    /// The dockable panes. Each can be dragged to another edge, tabbed with
    /// another, torn off into a floating window and closed -- which is what
    /// QDockWidget gave and what a panel in a sizer would not. Their
    /// arrangement is saved as a perspective; see persistState().
    EqualizerPanel* equalizer_ = nullptr;
    InfoPanel*      info_      = nullptr;
    LyricsPanel*    lyrics_    = nullptr;
    SpectrumPanel*  spectrum_  = nullptr;
#ifdef XPCOG_HAVE_SC55_PANEL
    /// The SC-55's front panel. Closed by default: it is one synthesiser of
    /// three, for one format among many, and a photograph of a 1993 sound
    /// module is not what someone who opened a music player asked to look at.
    Sc55Panel* sc55_ = nullptr;
#endif

    /// Built the first time it is asked for. Null until then: most sessions
    /// never open it, and it holds a seek bar that would otherwise be
    /// following the position for nobody.
    MiniFrame* mini_ = nullptr;

    /// The tray icon, or the Dock menu on macOS. Never null, but its methods
    /// do nothing where the platform has no notification area.
    std::unique_ptr<StatusPresence> presence_;

    /// So the "still running" notification appears once, not on every close.
    bool trayHintShown_ = false;

    /// Set while shutting down, so the close handler knows this is a real quit
    /// rather than a close to be intercepted. Without it, close-to-tray would
    /// make Quit hide the window and leave the application running with no way
    /// to stop it.
    bool quitting_ = false;

    SeekBar*      seekBar_    = nullptr;
    wxSlider*     volume_     = nullptr;
    SpeedPanel*   speedPanel_ = nullptr;
    wxSearchCtrl* filter_     = nullptr;
    wxStaticText* clock_      = nullptr;
    wxGauge*      scanBar_    = nullptr;
    wxBitmapButton* scanCancel_ = nullptr;

    /// The transport and the pane toggles. A real toolbar rather than a row of
    /// wxBitmapButtons: a tool posts wxEVT_TOOL, which *is* wxEVT_MENU, so it
    /// reaches the same handler the menu item does with nothing translating for
    /// it -- and wxToolBarBase::UpdateWindowUI walks its own tools every idle,
    /// which is what puts the pane toggles' pressed state on the same
    /// EVT_UPDATE_UI handlers the View menu's ticks already come from.
    ///
    /// The frame's own toolbar, so the frame places it above the client area and
    /// nothing in the sizer shares a row with it. Borrowed, not owned: the frame
    /// destroys it with its other children, and clears this pointer when it does.
    ///
    /// Tools are addressed by command id, so there is no parallel vector to keep
    /// in step with toolbarLayout() -- which is what the old button list was.
    wxToolBar* toolBar_ = nullptr;

    /// The OS's Now Playing entry and media keys. Never null -- platforms
    /// without an implementation get a base-class instance that does nothing.
    std::unique_ptr<platform::MediaIntegration> media_;
    /// The position last pushed to the OS. It extrapolates from the rate, so
    /// pushing every transport tick would be four rewrites a second for a
    /// display that is already counting correctly on its own.
    double mediaPosition_ = -1.0;

    /// The taskbar button's overlay badge and progress bar. Never null; the base
    /// class does nothing where the platform has no such surface.
    std::unique_ptr<platform::TaskbarIntegration> taskbar_;

    /// One scan at a time: the PluginCache the scans share is not synchronised.
    /// Requests arriving while one runs wait their turn, which also keeps a burst
    /// of drops landing in the order they were dropped.
    struct ScanRequest {
        std::vector<Url> inputs;
        int              atRow = -1;

        /// A reload rather than an addition: the results are merged onto the
        /// entries that are already there instead of being inserted. Both go
        /// through the same queue because they contend for the same PluginCache,
        /// and a reload starting mid-scan is the race that queue exists to stop.
        bool reload = false;

        /// Applied to the scan's results just before they are inserted.
        ///
        /// Exists for the Cog import, which has to put back what the store knew
        /// and the files do not -- ReplayGain, the queue, where the last session
        /// had got to -- and then match play counts, all of which can only
        /// happen once the scanner has read the tags. Empty for every other
        /// scan, which wants the results exactly as they came.
        std::function<void(std::vector<PlaylistEntry>&)> decorate;
    };
    std::unique_ptr<ScanTask> scan_;
    std::vector<ScanRequest>  pendingScans_;

    /// What the last Cog import matched, filled by the scan's decorator and read
    /// by addScannedEntries so the summary can say it. Held here rather than
    /// captured, because the two run at different times on the same thread.
    std::optional<CogPlayCountReport> cogImportSummary_;
    std::size_t                       cogImportFileReferences_ = 0;

    /// The duration of the audible track, remembered so the clock can show the
    /// scrubbed time against it without asking the controller mid-drag.
    double duration_ = 0.0;

    /// Which entry is audible, so a mid-stream tag change can tell whether it
    /// affects the now-playing display or only a row.
    TrackId currentTrack_ = kInvalidTrackId;

    /// Declared last; see the class comment. Members are destroyed in reverse
    /// order, so this goes first.
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::app
