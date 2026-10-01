// The player window.
//
// The GTK counterpart of app-winui/src/MainWindow.hpp, and a much smaller thing than
// that file was, because the session owns everything that is not a window --
// see uicore/src/Session.hpp. What is here is what draws: the transport, the
// seek bar and the clock, the volume, the filter, the status line, and the
// handlers that turn the session's signals into those widgets changing.
//
// Two conventions carried over. **Every connection this window makes lives
// in wireUp()**, so "what updates when the track changes" has one place to
// look. **Commands are one switch**: every action installed from the command
// table lands in onCommand() with its CommandId, the way every wx surface
// posts one id to one Bind.
//
// The widgets are borrowed. The builder owns the tree until the window is
// realised and the window owns it afterwards; this object holds pointers for
// the length of the window's life and nothing it could dangle on after.

#pragma once

#include "Actions.hpp"
#include "Commands.hpp"
#include "FileTreePane.hpp"
#include "Glib.hpp"
#include "MiniWindow.hpp"
#include "Panes.hpp"
#include "Presence.hpp"
#include "SeekBar.hpp"
#include "Visualizers.hpp"
#ifdef XPCOG_HAVE_SC55_PANEL
#include "Sc55View.hpp"
#endif
#include "PlaylistPane.hpp"
#include "PreferencesDialog.hpp"
#include "Session.hpp"

#include <adwaita.h>

#include <memory>
#include <string>
#include <vector>

namespace xpcog::gtk {

class MainWindow {
public:
    MainWindow(AdwApplication* application, app::Session& session);
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    [[nodiscard]] GtkWindow* window() const { return window_; }

    /// Brings the window to the front, un-minimising it if need be.
    void present();

    /// Saves and ends the application, whatever is open over the window.
    void quit();

    /// Adds files, folders, playlists or cue sheets. Public so the command
    /// line and a second launch reach the same code path the menu does.
    void openUrls(const std::vector<Url>& urls);

    /// The crash-reporting question, on the first launch of a build that can
    /// send reports. Asked once, ever, whichever way it was answered. The
    /// application asks once the window is on screen.
    void askCrashReportingConsent();

private:
    void buildUi(AdwApplication* application);
    void wireUp();

    /// Every command, from every surface, arrives here.
    void onCommand(app::CommandId id);

    /// Pushes the state a wx window would answer an EVT_UPDATE_UI with: the
    /// order radios from the settings, the check marks from the panes, the
    /// enabled state from the stack and the transport.
    void refreshActionState();

    void onTrackChanged(TrackId id, const PlaylistEntry* entry, bool looping);
    void onPlaybackStateChanged(bool playing, bool paused);
    void onPositionChanged(double seconds, double duration);
    void onEffectApplied(app::Effect effect, const std::string& key);

    void openFiles();
    void openFolder();
    void showAbout();
    /// Opens Preferences on `page`, or brings the open one forward.
    void showPreferences(PreferencesPage page);

    /// Switches between the full window and the mini player. A mode, as in
    /// Cog: one is shown and the other hidden, never both.
    void setMiniMode(bool mini);

    /// Pops the playlist's context menu at list coordinates.
    void showPlaylistMenu(double x, double y);

    /// Shows or hides the folder browser, which is the split view's sidebar.
    void showFileTree(bool show);

    /// Shows the right sidebar on `page` ("info" or "lyrics"), or hides it
    /// when that page is the one showing -- what toggling a pane means when
    /// two panes share one sidebar.
    void togglePanel(const char* page);
    /// Shows or hides one section of the tools strip by name.
    void showTool(const std::string& name, bool show);

    /// Which track the Info and Lyrics panes should be describing: the
    /// selection when there is one, the playing track otherwise, or the
    /// playing track always -- `panelFollowMode`'s rule, Cog's default first.
    [[nodiscard]] TrackId panelTrackId() const;
    /// Redraws the two panes, cheaply when they are hidden.
    void refreshPanels();
    /// Pushes the check marks for the panes to the actions.
    void refreshPaneState();
    /// Starts or stops the painted panes' clocks: each runs while its section
    /// is shown and something is playing, and not otherwise.
    void refreshVisualizers();
    /// Re-reads the waveform seek bar's mode and style into both bars.
    void applyWaveformSetting();

    /// Moves the selected files to the trash and takes them out of the
    /// playlist. Asks first, once, unless the listener has said not to.
    void trashSelected();

    /// Remembers the window's size and whether it is maximised, and the
    /// column widths, under this frontend's own keys.
    void persistState();
    void restoreState();

    void setStatusText(const std::string& text);
    void setClock(double seconds, double duration);

    app::Session& session_;

    GObjectPtr<GtkBuilder>   builder_;
    GtkWindow*               window_       = nullptr;
    AdwWindowTitle*          windowTitle_  = nullptr;
    GtkMenuButton*           addButton_    = nullptr;
    GtkMenuButton*           primaryButton_ = nullptr;
    GtkButton*               playButton_   = nullptr;
    GtkLabel*                clockLabel_   = nullptr;
    GtkScale*                volumeScale_  = nullptr;
    GtkAdjustment*           volumeAdjustment_ = nullptr;
    GtkSearchEntry*          filterEntry_  = nullptr;
    GtkLabel*                statusLabel_  = nullptr;
    GtkLabel*                nowPlayingLabel_ = nullptr;
    GtkWidget*               scanBox_      = nullptr;
    GtkProgressBar*          scanBar_      = nullptr;
    GtkButton*               scanCancel_   = nullptr;
    AdwBin*                  contentBin_   = nullptr;
    AdwOverlaySplitView*     splitView_    = nullptr;
    AdwBin*                  fileTreeBin_  = nullptr;
    AdwOverlaySplitView*     panelsView_   = nullptr;
    AdwViewStack*            panelsStack_  = nullptr;
    AdwBin*                  infoBin_      = nullptr;
    AdwBin*                  lyricsBin_    = nullptr;
    GtkPaned*                toolsPaned_   = nullptr;
    AdwBin*                  toolsBin_     = nullptr;

    std::unique_ptr<Actions>      actions_;
    std::unique_ptr<SeekBar>      seekBar_;
    std::unique_ptr<PlaylistPane> playlist_;
    std::unique_ptr<FileTreePane> fileTree_;
    std::unique_ptr<InfoPane>      info_;
    std::unique_ptr<LyricsPane>    lyrics_;
    std::unique_ptr<EqualizerPane> equalizer_;
    std::unique_ptr<SpeedPane>     speed_;
    std::unique_ptr<SpectrumView>  spectrum_;
    std::unique_ptr<OscilloscopeView> scope_;
#ifdef XPCOG_HAVE_SC55_PANEL
    std::unique_ptr<Sc55View> sc55_;
#endif
    std::unique_ptr<ToolsStrip>    tools_;
    /// The preferences dialog while it is open; dropped when it closes.
    std::unique_ptr<PreferencesDialog> preferences_;
    Subscription                       preferencesChanged_;

    /// The tray icon and the notifications. Never null; its methods do
    /// nothing where the session has no tray.
    std::unique_ptr<Presence> presence_;

    /// Built the first time it is asked for. Null until then: most sessions
    /// never open it.
    std::unique_ptr<MiniWindow> mini_;
    /// The playlist's context menu, parented to the list. Built once.
    GtkWidget*                    playlistMenu_ = nullptr;

    /// The duration of the audible track, so the clock can show a scrub
    /// against it without asking the controller mid-drag.
    double duration_ = 0.0;
    /// Set while this window is writing the volume scale, so the
    /// value-changed handler knows a change is not the listener's.
    bool settingScales_ = false;

    /// Set while shutting down, so the close handler knows this is a real
    /// quit rather than a close to be intercepted.
    bool quitting_ = false;

    std::vector<Connection>   connections_;
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::gtk
