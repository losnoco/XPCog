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
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/ScanTask.hpp"
#include "xpcog/platform/MediaIntegration.hpp"
#include "xpcog/platform/TaskbarIntegration.hpp"

#include <wx/frame.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxDataViewCtrl;
class wxGauge;
class wxBitmapButton;
class wxSearchCtrl;
class wxSlider;
class wxSplitterWindow;
class wxStaticText;

namespace xpcog::app {

class EqualizerPanel;
class FileTree;
class InfoPanel;
class MiniFrame;
class PlaylistDataModel;
class Sc55Panel;
class SeekBar;
class SpectrumPanel;

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
    void buildTransport(wxWindow* parent);
    /// Every connection this window owns, in one place.
    void wireUp();
    void bindCommands();
    void bindUpdateUi();

    void openFiles();
    void openFolder();
    void openUrl();
    void savePlaylistAs();
    void showPreferences();
    void showAbout();

    /// A setting changed and something has to be told. Shared by the
    /// preferences dialog and the equaliser panel, which both publish the same
    /// keys -- and the equaliser's is the case that must not be forgotten,
    /// since a band that does not reach the engine is a slider that does
    /// nothing.
    void onSettingChanged(const std::string& key);

    /// Switches between the full window and the mini player. A mode, as in
    /// Cog: one is shown and the other hidden, never both.
    void setMiniMode(bool mini);

    /// Redraws the info panel. Cog's rule (InfoWindowController): the playlist
    /// selection when there is one, the playing track otherwise. Cheap to call
    /// from anywhere, because it returns immediately while the panel is hidden
    /// -- which is most of the time, and matters because metadata arriving
    /// during a scan would otherwise redraw twenty fields per file.
    void refreshInfo();

    /// Shows or hides one of the side panels, and keeps the layout sane.
    void togglePanel(wxWindow* panel, bool show);

    void addUrls(const std::vector<Url>& urls, int atRow = -1);

    /// Starts the next queued scan, if any and if none is running.
    void pumpScanQueue();
    void addScannedEntries(std::vector<PlaylistEntry> entries, int atRow, bool cancelled);

    void onPositionChanged(double seconds, double duration);
    void onCurrentTrackChanged(TrackId id);
    void onPlaybackStateChanged(bool playing, bool paused);

    void removeSelected();
    void enqueueSelected();
    void activateRow(unsigned int row);

    [[nodiscard]] std::vector<TrackId> selectedTracks() const;

    /// Tells the OS what is playing. Reads the artwork from the library, which
    /// is why it lives here and not in PlaybackController.
    void publishNowPlaying(TrackId id);

    /// Saves everything that has to outlive the session: window geometry, the
    /// splitter, the file tree's root, the playlist, and the settings store.
    void persistState();
    void restoreState();

    [[nodiscard]] std::string statusSummary() const;

    void setStatusText(const std::string& text);

    const PluginRegistry& registry_;
    Settings&             settings_;
    Dispatcher            dispatch_;

    Playlist                 playlist_;
    PluginCache              cache_;
    std::unique_ptr<Library> library_;
    PlaylistView             view_;
    UndoStack                undo_;

    std::unique_ptr<PlaybackController> playback_;

    wxSplitterWindow* splitter_ = nullptr;
    FileTree*         tree_     = nullptr;
    wxDataViewCtrl*   list_     = nullptr;
    /// Reference-counted by the control, so this is a borrowed pointer and must
    /// not be deleted here.
    PlaylistDataModel* model_ = nullptr;

    /// The three optional panels. Shown and hidden rather than docked: wxAUI
    /// draws its own captions on every platform, which on macOS especially
    /// looks like a Windows application, and none of these needs tearing off.
    EqualizerPanel* equalizer_ = nullptr;
    InfoPanel*      info_      = nullptr;
    SpectrumPanel*  spectrum_  = nullptr;
#ifdef XPCOG_HAVE_SC55_PANEL
    /// The SC-55's front panel. Hidden, and not built into the default
    /// layout: it is one synthesiser of three, for one format among many, and
    /// a photograph of a 1993 sound module is not what someone who opened a
    /// music player asked to look at.
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
    wxSearchCtrl* filter_     = nullptr;
    wxStaticText* clock_      = nullptr;
    wxGauge*      scanBar_    = nullptr;
    wxBitmapButton* scanCancel_ = nullptr;

    std::vector<wxBitmapButton*> transportButtons_;

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
    };
    std::unique_ptr<ScanTask> scan_;
    std::vector<ScanRequest>  pendingScans_;

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
