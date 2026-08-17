// The player window. Replaces the 3,181 lines of Cog's Base.lproj/MainMenu.xib.
//
// Every connect() this window makes lives in wireUp(). That convention is the
// answer to Cog's 190 bindings scattered across a dozen XIBs: when the question
// is "what updates when the track changes", there is exactly one place to look.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/platform/MediaIntegration.hpp"

#include <QList>
#include <QMainWindow>
#include <QUrl>

#include <memory>
#include <vector>

class QDockWidget;
class QLabel;
class QMenu;
class QToolBar;
class QLineEdit;
class QSlider;
class QProgressBar;
class QTableView;
class QToolButton;
class QUndoStack;

namespace xpcog::app {

class ActionRegistry;
class FileTree;
class ScanTask;
class SeekSlider;
class PlaybackController;
class PlaylistModel;
class PlaylistProxyModel;
class SpectrumWidget;
class StatusPresence;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(const PluginRegistry& registry, Settings& settings,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Adds files, folders, playlists or cue sheets. Public so the command line
    /// and the OS's open-document event reach the same code path the menu does.
    void openUrls(const QList<QUrl>& urls);

protected:
    void closeEvent(QCloseEvent* event) override;

    /// The menu QMainWindow offers on a right-click, listing toolbars and docks
    /// so they can be hidden. The transport is removed from it: hiding the only
    /// play button leaves a window with no way to start playback and no obvious
    /// way back, since the menu that would restore it is the one you have to
    /// find by right-clicking exactly the right empty strip.
    [[nodiscard]] QMenu* createPopupMenu() override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void buildMenus();
    /// Every connection this window owns, in one place.
    void wireUp();

    void openFiles();
    void openFolder();
    void showPreferences();
    void savePlaylistAs();
    void addUrls(const QList<QUrl>& urls, int atRow = -1);

    /// Starts the next queued scan, if any and if none is running.
    void pumpScanQueue();
    void addScannedEntries(std::vector<PlaylistEntry> entries, int atRow, bool cancelled);

    void onPositionChanged(double seconds, double duration);
    void onCurrentTrackChanged(TrackId id);
    void onPlaybackStateChanged(bool playing, bool paused);
    void onRowActivated(const QModelIndex& index);

    void removeSelected();
    void enqueueSelected();

    /// Keeps the Undo and Redo commands' enabled state and labels in step with
    /// the stack. Qt's QUndoStack::createUndoAction() would do this, but it
    /// makes its own QAction, and every command in this window is one the
    /// registry owns.
    void refreshUndoActions();

    /// Tells the OS what is playing. Reads the artwork from the library, which
    /// is why it lives here and not in PlaybackController.
    void publishNowPlaying(TrackId id);

    [[nodiscard]] QString statusSummary() const;

    const PluginRegistry& registry_;
    Settings&             settings_;

    Playlist                            playlist_;
    PluginCache                         cache_;
    std::unique_ptr<Library>            library_;
    std::unique_ptr<PlaybackController> playback_;

    ActionRegistry*     actions_ = nullptr;
    PlaylistModel*      model_   = nullptr;
    PlaylistProxyModel* proxy_   = nullptr;
    QUndoStack*         undo_    = nullptr;

    QToolBar*   transport_ = nullptr;
    FileTree*   tree_     = nullptr;
    QTableView* view_     = nullptr;
    SeekSlider* seekBar_  = nullptr;
    QSlider*    volume_   = nullptr;
    QLineEdit*  filter_   = nullptr;
    QLabel*     nowPlaying_ = nullptr;
    QLabel*     clock_    = nullptr;

    /// The OS's Now Playing entry and media keys. Never null -- platforms
    /// without an implementation get a base-class instance that does nothing.
    platform::MediaIntegration* media_ = nullptr;
    /// The position last pushed to the OS. It extrapolates from the rate, so
    /// pushing every transport tick would be four rewrites a second for a
    /// display that is already counting correctly on its own.
    double mediaPosition_ = -1.0;

    /// The tray icon, or the Dock menu on macOS. Never null, but its methods do
    /// nothing where the platform has no notification area.
    StatusPresence* presence_ = nullptr;

    /// The spectrum panel and the dock it lives in. The dock is held because the
    /// View menu item and the dock's own close button both change its visibility
    /// and have to stay in step.
    SpectrumWidget* spectrum_     = nullptr;
    QDockWidget*    spectrumDock_ = nullptr;

    QProgressBar* scanBar_    = nullptr;
    QToolButton*  scanCancel_ = nullptr;

    /// One scan at a time: the PluginCache the scans share is not synchronised.
    /// Requests arriving while one runs wait their turn, which also keeps a
    /// burst of drops landing in the order they were dropped.
    struct ScanRequest {
        std::vector<Url> inputs;
        int              atRow = -1;
    };
    ScanTask*                scan_ = nullptr;
    std::vector<ScanRequest> pendingScans_;

    /// The duration of the audible track, remembered so the clock can show the
    /// scrubbed time against it without asking the controller mid-drag.
    double duration_ = 0.0;
};

}  // namespace xpcog::app
