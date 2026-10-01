#include "MainWindow.hpp"

#include "Accelerators.hpp"
#include "Dialogs.hpp"
#include "Menus.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/OpenUrl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <utility>

namespace xpcog::gtk {

using app::CommandId;
using app::Effect;

namespace {

constexpr const char* kWindowResource = "/co/losno/XPCog/ui/window.ui";

template <typename T>
T* object(GtkBuilder* builder, const char* id) {
    GObject* found = gtk_builder_get_object(builder, id);
    if (found == nullptr) {
        g_error("%s has no object named %s", kWindowResource, id);
    }
    return reinterpret_cast<T*>(found);
}

/// The URLs a file dialog answered with.
std::vector<Url> urlsFrom(GListModel* files) {
    std::vector<Url> urls;
    if (files == nullptr) {
        return urls;
    }
    const guint count = g_list_model_get_n_items(files);
    for (guint i = 0; i < count; ++i) {
        auto file = GObjectPtr<GFile>::adopt(G_FILE(g_list_model_get_item(files, i)));
        GStr path(g_file_get_path(file.get()));
        if (path) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path{path.c_str()}));
        } else if (GStr uri(g_file_get_uri(file.get())); uri) {
            if (std::optional<Url> url = Url::parse(uri.c_str())) {
                urls.push_back(*url);
            }
        }
    }
    return urls;
}

}  // namespace

MainWindow::MainWindow(AdwApplication* application, app::Session& session)
    : session_(session) {
    buildUi(application);
    wireUp();
    refreshActionState();

    // Anything the session said before there was a status bar to say it on.
    if (!session_.lastStatus().empty()) {
        setStatusText(session_.lastStatus());
    }
}

MainWindow::~MainWindow() {
    // A popover parented by hand is unparented by hand, before the list it
    // is parented to goes.
    if (playlistMenu_ != nullptr) {
        gtk_widget_unparent(playlistMenu_);
        playlistMenu_ = nullptr;
    }
}

void MainWindow::buildUi(AdwApplication* application) {
    builder_ = GObjectPtr<GtkBuilder>::adopt(gtk_builder_new_from_resource(kWindowResource));

    window_           = object<GtkWindow>(builder_.get(), "main_window");
    windowTitle_      = object<AdwWindowTitle>(builder_.get(), "window_title");
    addButton_        = object<GtkMenuButton>(builder_.get(), "add_button");
    primaryButton_    = object<GtkMenuButton>(builder_.get(), "primary_button");
    playButton_       = object<GtkButton>(builder_.get(), "play_button");
    clockLabel_       = object<GtkLabel>(builder_.get(), "clock_label");
    volumeScale_      = object<GtkScale>(builder_.get(), "volume_scale");
    volumeAdjustment_ = object<GtkAdjustment>(builder_.get(), "volume_adjustment");
    filterEntry_      = object<GtkSearchEntry>(builder_.get(), "filter_entry");
    statusLabel_      = object<GtkLabel>(builder_.get(), "status_label");
    nowPlayingLabel_  = object<GtkLabel>(builder_.get(), "now_playing_label");
    scanBox_          = object<GtkWidget>(builder_.get(), "scan_box");
    scanBar_          = object<GtkProgressBar>(builder_.get(), "scan_bar");
    scanCancel_       = object<GtkButton>(builder_.get(), "scan_cancel");
    contentBin_       = object<AdwBin>(builder_.get(), "content_bin");
    splitView_        = object<AdwOverlaySplitView>(builder_.get(), "split_view");
    fileTreeBin_      = object<AdwBin>(builder_.get(), "file_tree_bin");
    panelsView_       = object<AdwOverlaySplitView>(builder_.get(), "panels_view");
    panelsStack_      = object<AdwViewStack>(builder_.get(), "panels_stack");
    infoBin_          = object<AdwBin>(builder_.get(), "info_bin");
    lyricsBin_        = object<AdwBin>(builder_.get(), "lyrics_bin");
    toolsPaned_       = object<GtkPaned>(builder_.get(), "tools_paned");
    toolsBin_         = object<AdwBin>(builder_.get(), "tools_bin");

    // A GtkWindow is owned by GTK from the moment it has an application:
    // gtk_window_destroy releases it, and the application's last window closing
    // is what ends the main loop.
    gtk_window_set_application(window_, GTK_APPLICATION(application));

    // The commands, as actions on this window: "win.play-pause" and the rest.
    actions_ = std::make_unique<Actions>(G_ACTION_MAP(window_),
                                         [this](CommandId id) { onCommand(id); });
    // And the one action that is not a command of the table: the shortcuts
    // dialog, which the wx window has no counterpart for.
    {
        GSimpleAction* shortcuts = g_simple_action_new("shortcuts", nullptr);
        connections_.push_back(Connection::to<void(GSimpleAction*, GVariant*)>(
            shortcuts, "activate",
            [this](GSimpleAction*, GVariant*) { showShortcutsDialog(GTK_WIDGET(window_)); }));
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(shortcuts));
        g_object_unref(shortcuts);
    }

    // The menus, from the same table the actions came from.
    gtk_menu_button_set_menu_model(addButton_, G_MENU_MODEL(buildAddMenu().get()));
    gtk_menu_button_set_menu_model(primaryButton_, G_MENU_MODEL(buildPrimaryMenu().get()));

    // The playlist, in the middle, and the folder browser beside it.
    playlist_ = std::make_unique<PlaylistPane>(session_.view(), session_.settings());
    adw_bin_set_child(contentBin_, playlist_->widget());
    fileTree_ = std::make_unique<FileTreePane>(session_.registry(), window_);
    adw_bin_set_child(fileTreeBin_, fileTree_->widget());

    // The panes: Info and Lyrics in the right sidebar, the rest in the strip
    // under the playlist. Names are the wx perspective's, for the reader who
    // knows those.
    info_ = std::make_unique<InfoPane>(session_.library());
    adw_bin_set_child(infoBin_, info_->widget());
    lyrics_ = std::make_unique<LyricsPane>([this] { return session_.playback().position(); });
    lyrics_->setLookup(session_.lyricsLookup());
    adw_bin_set_child(lyricsBin_, lyrics_->widget());

    // The seek bar, into the slot the transport row leaves for it.
    seekBar_ = std::make_unique<SeekBar>();
    gtk_box_append(GTK_BOX(object<GtkBox>(builder_.get(), "seek_slot")), seekBar_->widget());

    tools_     = std::make_unique<ToolsStrip>();
    equalizer_ = std::make_unique<EqualizerPane>(session_.settings());
    speed_     = std::make_unique<SpeedPane>(session_.settings());
    spectrum_  = std::make_unique<SpectrumView>(session_.playback().tap(), session_.settings());
    scope_     = std::make_unique<OscilloscopeView>(session_.playback().tap(), session_.settings());
    tools_->addSection("spectrum", app::commandLabel(CommandId::ViewSpectrum),
                       spectrum_->widget());
    tools_->addSection("scope", app::commandLabel(CommandId::ViewOscilloscope),
                       scope_->widget());
    tools_->addSection("equalizer", app::commandLabel(CommandId::ViewEqualizer),
                       equalizer_->widget());
    tools_->addSection("speed", app::commandLabel(CommandId::ViewSpeed),
                       speed_->widget());
#ifdef XPCOG_HAVE_SC55_PANEL
    sc55_ = std::make_unique<Sc55View>([this] { return session_.playback().position(); });
    tools_->addSection("sc55", app::commandLabel(CommandId::ViewSc55Panel), sc55_->widget());
#else
    // Without the MIDI build there is no emulator to render a panel state;
    // the section exists so the layout keys mean the same thing, and the
    // action is disabled.
    tools_->addSection("sc55", app::commandLabel(CommandId::ViewSc55Panel), gtk_label_new(""));
#endif
    adw_bin_set_child(toolsBin_, tools_->widget());

    // Its context menu, parented to the list so it can point at a row.
    playlistMenu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(buildPlaylistMenu().get()));
    gtk_popover_set_has_arrow(GTK_POPOVER(playlistMenu_), FALSE);
    gtk_widget_set_halign(playlistMenu_, GTK_ALIGN_START);
    gtk_widget_set_parent(playlistMenu_, playlist_->listWidget());

    // The volume, seeded from the setting the engine was seeded from.
    settingScales_ = true;
    gtk_adjustment_set_value(volumeAdjustment_, session_.settings().Volume() * 100.0);
    settingScales_ = false;

    // The window's icon, from the resource the tray reads too.
    gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                     "/co/losno/XPCog/icons");
    gtk_window_set_icon_name(window_, "co.losno.XPCog");

    presence_ = std::make_unique<Presence>(&postToMainContext);

    restoreState();
}

void MainWindow::wireUp() {
    const auto observe = [this](auto& signal, auto handler) {
        subscriptions_.push_back(signal.connect(std::move(handler)));
    };

    // --- the session --------------------------------------------------------
    observe(session_.status, [this](const std::string& text) { setStatusText(text); });
    observe(session_.positionChanged,
            [this](double seconds, double duration) { onPositionChanged(seconds, duration); });
    observe(session_.trackChanged, [this](TrackId id, const PlaylistEntry* entry, bool looping) {
        onTrackChanged(id, entry, looping);
    });
    observe(session_.playbackStateChanged,
            [this](bool playing, bool paused) { onPlaybackStateChanged(playing, paused); });
    observe(session_.effectApplied,
            [this](Effect effect, const std::string& key) { onEffectApplied(effect, key); });
    observe(session_.scanStarted, [this] {
        gtk_progress_bar_set_fraction(scanBar_, 0.0);
        gtk_widget_set_visible(scanBox_, TRUE);
    });
    observe(session_.scanProgress, [this](int done, int total) {
        if (total > 0) {
            gtk_progress_bar_set_fraction(scanBar_, static_cast<double>(done) / total);
        } else {
            gtk_progress_bar_pulse(scanBar_);
        }
    });
    observe(session_.scanFinished, [this] {
        gtk_widget_set_visible(scanBox_, FALSE);
        refreshActionState();
    });
    observe(session_.raiseRequested, [this] { present(); });
    observe(session_.quitRequested, [this] { quit(); });
    observe(session_.volumeChanged, [this](double gain) {
        settingScales_ = true;
        gtk_adjustment_set_value(volumeAdjustment_, gain * 100.0);
        settingScales_ = false;
        if (mini_) {
            mini_->setVolume(gain);
        }
    });
    observe(session_.undo().changed, [this] { refreshActionState(); });
    observe(session_.revealRequested, [this](TrackId id) { playlist_->revealTrack(id); });
    observe(session_.announceTrack,
            [this](const std::string& title, const std::string& body,
                   const std::shared_ptr<const std::vector<std::byte>>& cover) {
                presence_->notify(title, body, cover);
            });

    // --- the tray ----------------------------------------------------------------------
    observe(presence_->showRequested, [this] { present(); });
    observe(presence_->commandActivated, [this](int id) { onCommand(static_cast<CommandId>(id)); });

    // --- the playlist -------------------------------------------------------------
    observe(playlist_->activated, [this](TrackId id) { session_.playback().playTrack(id); });
    observe(playlist_->selectionChanged, [this] {
        refreshActionState();
        refreshPanels();
    });
    observe(playlist_->contextMenuRequested, [this](double x, double y) { showPlaylistMenu(x, y); });
    observe(playlist_->filesDropped,
            [this](const std::vector<Url>& urls, int row) { session_.addUrls(urls, row); });

    // --- the panes ----------------------------------------------------------------------
    observe(equalizer_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });
    observe(speed_->settingChanged, [this](const std::string& key) { session_.settingChanged(key); });
    observe(speed_->settingsRequested, [this] { showPreferences(PreferencesPage::PitchTempo); });
    observe(tools_->closeRequested, [this](const std::string& name) { showTool(name, false); });
    observe(session_.tracksUpdated, [this] { refreshPanels(); });
    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        panelsView_, "notify::show-sidebar", [this](GObject*, GParamSpec*) {
            refreshPaneState();
            refreshPanels();
        }));
    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        panelsStack_, "notify::visible-child-name", [this](GObject*, GParamSpec*) {
            refreshPaneState();
            refreshPanels();
        }));

    // --- the folder browser --------------------------------------------------------
    observe(fileTree_->activated, [this](const std::vector<Url>& urls) { session_.addUrls(urls); });
    observe(fileTree_->rootChosen, [this] {
        // Choosing what to look at and then not being shown it would read as
        // the dialog having done nothing.
        showFileTree(true);
    });
    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        splitView_, "notify::show-sidebar", [this](GObject*, GParamSpec*) {
            actions_->setChecked(CommandId::ViewFileTree,
                                 adw_overlay_split_view_get_show_sidebar(splitView_));
        }));

    // --- the transport row -----------------------------------------------------
    //
    // The seek bar: a drag scrubs, with the clock following, and seeks on
    // release; a click seeks at once. Both are the widget's.
    observe(seekBar_->seekRequested, [this](double seconds) { session_.playback().seek(seconds); });
    observe(seekBar_->scrubbed, [this](double seconds) { setClock(seconds, duration_); });
    observe(session_.waveformUpdated,
            [this](const std::shared_ptr<const WaveformSummary>& summary) {
                seekBar_->setWaveform(summary);
                if (mini_) {
                    mini_->setWaveform(summary);
                }
            });
    applyWaveformSetting();

    // --- the painted panes -------------------------------------------------------
    //
    // The sample rate is what the spectrum's band table is built against,
    // and it is not known until a device has been negotiated -- which happens
    // when a track starts, not when the pane is created.
    observe(session_.playbackStateChanged, [this](bool, bool) {
        spectrum_->setSampleRate(session_.playback().sampleRate());
        scope_->setSampleRate(session_.playback().sampleRate());
        refreshVisualizers();
    });
    observe(spectrum_->settingsRequested, [this] { showPreferences(PreferencesPage::Visualizers); });
    observe(scope_->settingsRequested, [this] { showPreferences(PreferencesPage::Visualizers); });
    // The visualisers' menus write settings; the change takes the same road
    // a Preferences change does, and ends back in the pane.
    observe(spectrum_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });
    observe(scope_->settingChanged, [this](const std::string& key) { session_.settingChanged(key); });

    connections_.push_back(Connection::to<void(GtkAdjustment*)>(
        volumeAdjustment_, "value-changed", [this](GtkAdjustment* adjustment) {
            if (settingScales_) {
                return;
            }
            const double gain = gtk_adjustment_get_value(adjustment) / 100.0;
            session_.settings().setVolume(gain);
            session_.setVolume(gain);
        }));

    connections_.push_back(Connection::to<void(GtkSearchEntry*)>(
        filterEntry_, "search-changed", [this](GtkSearchEntry* entry) {
            session_.view().setFilter(gtk_editable_get_text(GTK_EDITABLE(entry)));
        }));

    connections_.push_back(Connection::to<void(GtkButton*)>(
        scanCancel_, "clicked", [this](GtkButton*) { session_.cancelScans(); }));

    // --- the window -----------------------------------------------------------
    connections_.push_back(Connection::to<gboolean(GtkWindow*)>(
        window_, "close-request", [this](GtkWindow*) -> gboolean {
            // Layout and playlist are saved whichever way this goes, and
            // before the window is gone: the size is read while it is up.
            persistState();
            session_.save();

            // Close to tray, where there is a tray and the listener asked for
            // it. The application keeps running with its window hidden; the
            // tray's Show row and a second launch both bring it back.
            Settings& settings = session_.settings();
            if (!quitting_ && settings.CloseToTray() && presence_->hasTray()) {
                gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
                if (!settings.TrayHideAnnounced()) {
                    settings.setTrayHideAnnounced(true);
                    presence_->notify(app::tr("XPCog is still running"),
                                      app::tr("Playback continues. Use the tray icon to bring the "
                                              "window back or to quit."),
                                      nullptr);
                }
                return TRUE;  // kept
            }
            presence_->remove();
            if (mini_) {
                mini_.reset();
            }
            return FALSE;  // and close
        }));

    // Files dropped anywhere on the window that the list did not take.
    GtkDropTarget* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    connections_.push_back(Connection::to<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop", [this](GtkDropTarget*, const GValue* value, double, double) -> gboolean {
            if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
                return FALSE;
            }
            std::vector<Url> urls;
            auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
            for (GSList* node = gdk_file_list_get_files(list); node != nullptr; node = node->next) {
                GStr path(g_file_get_path(G_FILE(node->data)));
                if (path) {
                    urls.push_back(Url::fromLocalPath(std::filesystem::path{path.c_str()}));
                }
            }
            if (urls.empty()) {
                return FALSE;
            }
            session_.addUrls(urls);
            return TRUE;
        }));
    gtk_widget_add_controller(GTK_WIDGET(window_), GTK_EVENT_CONTROLLER(target));
}

// --- commands -------------------------------------------------------------------

void MainWindow::onCommand(CommandId id) {
    Settings& settings = session_.settings();
    switch (id) {
        case CommandId::FileOpen:
            openFiles();
            break;
        case CommandId::FileOpenFolder:
            openFolder();
            break;
        case CommandId::FileOpenUrl:
            showOpenUrlDialog(window_, settings, [this](const Url& url) { session_.addUrls({url}); });
            break;
        case CommandId::FileSavePlaylist:
            showSavePlaylistDialog(window_, session_, {});
            break;
        case CommandId::PlaylistSaveSelection: {
            // There is no point asking for a filename for an empty selection.
            std::vector<TrackId> selection = playlist_->selectedTracks();
            if (!selection.empty()) {
                showSavePlaylistDialog(window_, session_, std::move(selection));
            }
            break;
        }
        case CommandId::FilePreferences:
            showPreferences(PreferencesPage::Playlist);
            break;
        case CommandId::FileQuit:
            quit();
            break;
        case CommandId::HelpAbout:
            showAbout();
            break;

        case CommandId::PlaybackPlayPause:
            session_.playback().playPause();
            break;
        case CommandId::PlaybackStop:
            session_.playback().stop();
            break;
        case CommandId::PlaybackNext:
            session_.playback().next();
            break;
        case CommandId::PlaybackPrevious:
            session_.playback().previous();
            break;

        case CommandId::EditUndo:
            session_.undo().undo();
            break;
        case CommandId::EditRedo:
            session_.undo().redo();
            break;
        case CommandId::EditSelectAll:
            playlist_->selectAll();
            break;
        case CommandId::EditScrollToCurrent:
            // Enabled whenever something is playing, which is not the same as
            // something being *visible*: a filter in the box can hide the
            // playing track, and then this has nothing to scroll to.
            if (!playlist_->revealTrack(session_.currentTrack())) {
                setStatusText(app::tr("The playing track is hidden by the filter"));
            }
            break;
        case CommandId::EditRemove:
            session_.commands().remove(playlist_->selectedTracks());
            break;
        case CommandId::EditRandomize:
            session_.commands().randomize();
            break;

        case CommandId::PlaybackEnqueue:
            session_.commands().setQueued(playlist_->selectedTracks(), true);
            break;
        case CommandId::PlaylistToggleQueued:
            session_.commands().toggleQueued(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistStopAfter:
            session_.commands().toggleStopAfter(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistSearchArtist:
        case CommandId::PlaylistSearchAlbum: {
            // Cog opens its Spotlight window here; the filter is what replaced
            // that window, so this fills it in, from the first selected row.
            const std::vector<TrackId> selected = playlist_->selectedTracks();
            const PlaylistEntry*       entry =
                selected.empty() ? nullptr : session_.playlist().find(selected.front());
            if (entry != nullptr) {
                const std::string& text =
                    id == CommandId::PlaylistSearchAlbum ? entry->album.str() : entry->artist.str();
                gtk_editable_set_text(GTK_EDITABLE(filterEntry_), text.c_str());
            }
            break;
        }
        case CommandId::PlaylistReloadInfo:
            session_.reloadTracks(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistResetPlayCount:
            session_.resetPlayCount(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistRemoveRating:
            session_.removeRating(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistReveal:
            session_.revealInFileManager(playlist_->selectedTracks());
            break;
        case CommandId::PlaylistTrash:
            trashSelected();
            break;

        case CommandId::OrderRepeatNone:
        case CommandId::OrderRepeatOne:
        case CommandId::OrderRepeatAlbum:
        case CommandId::OrderRepeatAll: {
            // The enum's values are Cog's stored ones, in order.
            settings.setRepeatMode(static_cast<int>(id) - static_cast<int>(CommandId::OrderRepeatNone));
            session_.settingChanged("repeat");
            actions_->setChoice(id);
            break;
        }
        case CommandId::OrderShuffleOff:
        case CommandId::OrderShuffleAlbums:
        case CommandId::OrderShuffleAll: {
            settings.setShuffleMode(static_cast<int>(id) - static_cast<int>(CommandId::OrderShuffleOff));
            session_.settingChanged("shuffle");
            actions_->setChoice(id);
            break;
        }

        case CommandId::ViewFollowSelection:
        case CommandId::ViewFollowPlayback:
            settings.setPanelFollowMode(id == CommandId::ViewFollowSelection ? 0 : 1);
            session_.settingChanged("panelFollowMode");
            actions_->setChoice(id);
            break;

        case CommandId::ViewFileTree:
            showFileTree(!adw_overlay_split_view_get_show_sidebar(splitView_));
            break;
        case CommandId::ViewInfo:
            togglePanel("info");
            break;
        case CommandId::ViewLyrics:
            togglePanel("lyrics");
            break;
        case CommandId::ViewTimedLyrics:
            // The pane reads the setting on every redraw, so a redraw is the
            // whole of what telling it takes.
            settings.setLyricsSynced(!settings.LyricsSynced());
            session_.settingChanged("lyricsSynced");
            actions_->setChecked(id, settings.LyricsSynced());
            break;
        case CommandId::ViewSpectrum:
            showTool("spectrum", !tools_->shown("spectrum"));
            break;
        case CommandId::ViewOscilloscope:
            showTool("scope", !tools_->shown("scope"));
            break;
        case CommandId::ViewEqualizer:
            showTool("equalizer", !tools_->shown("equalizer"));
            break;
        case CommandId::ViewSpeed:
            showTool("speed", !tools_->shown("speed"));
            break;
        case CommandId::ViewSc55Panel:
            showTool("sc55", !tools_->shown("sc55"));
            break;
        case CommandId::ViewDockPanes:
            // Not offered here (offeredHere()): no pane can float, so there
            // is nothing to dock. Listed so the switch stays exhaustive.
            break;
        case CommandId::ViewMiniPlayer:
            setMiniMode(!(mini_ && mini_->visible()));
            break;
        case CommandId::ViewFileTreeRoot:
            fileTree_->chooseRootPath();
            break;

        case CommandId::ViewWaveform:
            settings.setWaveformSeekBar(!settings.WaveformSeekBar());
            session_.settingChanged("waveformSeekBar");
            actions_->setChecked(id, settings.WaveformSeekBar());
            break;

        case CommandId::FirstWidgetId:
            // Not a command. Listed so the switch is exhaustive: a command
            // added to the table without an arm here is a compiler warning,
            // not a menu item that does nothing.
            break;
    }
}

void MainWindow::refreshActionState() {
    const Settings& settings = session_.settings();
    const auto repeat = static_cast<int>(CommandId::OrderRepeatNone) +
                        std::clamp(settings.RepeatMode(), 0, 3);
    actions_->setChoice(static_cast<CommandId>(repeat));
    const auto shuffle = static_cast<int>(CommandId::OrderShuffleOff) +
                         std::clamp(settings.ShuffleMode(), 0, 2);
    actions_->setChoice(static_cast<CommandId>(shuffle));
    actions_->setChoice(settings.PanelFollowMode() == 0 ? CommandId::ViewFollowSelection
                                                        : CommandId::ViewFollowPlayback);
    actions_->setChecked(CommandId::ViewWaveform, settings.WaveformSeekBar());
    actions_->setChecked(CommandId::ViewTimedLyrics, settings.LyricsSynced());

    actions_->setEnabled(CommandId::EditUndo, session_.undo().canUndo());
    actions_->setEnabled(CommandId::EditRedo, session_.undo().canRedo());

    const bool anything = !session_.playlist().empty();
    actions_->setEnabled(CommandId::PlaybackPlayPause, anything);
    actions_->setEnabled(CommandId::PlaybackNext, anything);
    actions_->setEnabled(CommandId::PlaybackPrevious, anything);
    actions_->setEnabled(CommandId::PlaybackStop, session_.playback().playing());
    actions_->setEnabled(CommandId::EditScrollToCurrent,
                         session_.currentTrack() != kInvalidTrackId);

    // Every command that acts on the selection, off while there is none --
    // and the two that need a file off for a selection of streams.
    const std::vector<TrackId> selected = playlist_->selectedTracks();
    const bool                 any      = !selected.empty();
    bool                       files    = false;
    for (const TrackId id : selected) {
        const PlaylistEntry* entry = session_.playlist().find(id);
        files = files || (entry != nullptr && entry->url.localPath().has_value());
    }
    for (const CommandId id : {CommandId::EditRemove, CommandId::PlaybackEnqueue,
                               CommandId::PlaylistToggleQueued, CommandId::PlaylistStopAfter,
                               CommandId::PlaylistSaveSelection, CommandId::PlaylistSearchArtist,
                               CommandId::PlaylistSearchAlbum, CommandId::PlaylistReloadInfo,
                               CommandId::PlaylistResetPlayCount, CommandId::PlaylistRemoveRating}) {
        actions_->setEnabled(id, any);
    }
    actions_->setEnabled(CommandId::PlaylistReveal, files);
    actions_->setEnabled(CommandId::PlaylistTrash, files);
    actions_->setEnabled(CommandId::PlaylistRemoveRating, any && session_.library() != nullptr);
    actions_->setEnabled(CommandId::EditRandomize, anything);
    actions_->setEnabled(CommandId::EditSelectAll, anything);
}

// --- the session's news -------------------------------------------------------

void MainWindow::onTrackChanged(TrackId id, const PlaylistEntry* entry, bool looping) {
    (void)id;
    (void)looping;
    const std::string text = entry != nullptr ? entry->display() : std::string{};

    // Not translated, and the dash is not decoration -- see MainFrame's
    // onTrackChanged for why the window title is not a message.
    gtk_window_set_title(window_, text.empty() ? "XPCog" : (text + " \xE2\x80\x94 XPCog").c_str());
    adw_window_title_set_title(windowTitle_, entry != nullptr ? entry->title().c_str() : "XPCog");
    std::string subtitle;
    if (entry != nullptr) {
        subtitle = entry->artist;
        if (!entry->album.empty()) {
            subtitle += subtitle.empty() ? "" : " \xE2\x80\x94 ";
            subtitle += entry->album;
        }
    }
    adw_window_title_set_subtitle(windowTitle_, subtitle.c_str());
    gtk_label_set_text(nowPlayingLabel_, text.c_str());

    const std::string title  = entry != nullptr ? entry->title() : std::string{};
    const std::string artist = entry != nullptr ? entry->artist : std::string{};
    presence_->setNowPlaying(title, artist);
    if (mini_) {
        mini_->setNowPlaying(title, artist);
    }

    refreshActionState();
    refreshPanels();
}

void MainWindow::onPlaybackStateChanged(bool playing, bool paused) {
    gtk_button_set_icon_name(playButton_, playing && !paused ? "media-playback-pause-symbolic"
                                                             : "media-playback-start-symbolic");
    presence_->setPlaybackState(playing, paused);
    if (mini_) {
        mini_->setPlaybackState(playing, paused);
    }
    if (!playing) {
        duration_ = 0.0;
        seekBar_->setDuration(0.0);
        setClock(0.0, 0.0);
    }
    refreshActionState();
}

void MainWindow::onPositionChanged(double seconds, double duration) {
    duration_ = duration;
    if (mini_ && mini_->visible()) {
        mini_->setPosition(seconds, duration);
    }
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);
    if (!seekBar_->scrubbing()) {
        setClock(seconds, duration);
    }
}

void MainWindow::onEffectApplied(Effect effect, const std::string& key) {
    (void)key;
    switch (effect) {
        case Effect::PlaylistMode:
            refreshActionState();
            break;
        case Effect::WaveformSeekBar:
            applyWaveformSetting();
            refreshActionState();
            break;
        case Effect::RefreshSpectrum:
            spectrum_->applySettings(session_.settings());
            break;
        case Effect::RefreshScope:
            scope_->applySettings(session_.settings());
            break;
        case Effect::EqualizerCurve:
            // A curve that arrived from somewhere other than these sliders --
            // the remote control, genre tracking. The sliders have to show it.
            equalizer_->refresh();
            break;
        case Effect::RefreshSpeed:
            speed_->refresh();
            break;
        case Effect::RefreshPanels:
            refreshActionState();
            refreshPanels();
            break;
        case Effect::OnlineLyrics:
            lyrics_->setLookup(session_.lyricsLookup());
            refreshPanels();
            break;
        case Effect::MiniFloating:
            // Nothing to do: GTK 4 has no keep-above, and docs/GTKPORT.md
            // records the mini player not floating as a regression.
            break;
        case Effect::ReloadDsp:
        case Effect::GenreEqualizer:
        case Effect::ReopenOutput:
        case Effect::Volume:
        case Effect::Scrobbler:
        case Effect::CrashReporter:
        case Effect::RestartRemote:
        case Effect::None:
        case Effect::Internal:
            // Nothing a window shows changes; the session did its half.
            // Volume's slider follows Session::volumeChanged rather than this.
            break;
    }
}

// --- dialogs ----------------------------------------------------------------------

void MainWindow::openFiles() {
    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
    gtk_file_dialog_set_title(dialog.get(), app::tr("Open Files").c_str());

    // The registry's extensions, as the dialog's filter, with everything as the
    // alternative -- a file the registry does not name may still be a playlist
    // or a cue sheet the containers know.
    auto filters = GObjectPtr<GListStore>::adopt(g_list_store_new(GTK_TYPE_FILE_FILTER));
    auto audio   = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
    gtk_file_filter_set_name(audio.get(), app::tr("Audio Files").c_str());
    for (const std::string& extension : session_.registry().allExtensions()) {
        gtk_file_filter_add_suffix(audio.get(), extension.c_str());
    }
    auto all = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
    gtk_file_filter_set_name(all.get(), app::tr("All Files").c_str());
    gtk_file_filter_add_pattern(all.get(), "*");
    g_list_store_append(filters.get(), audio.get());
    g_list_store_append(filters.get(), all.get());
    gtk_file_dialog_set_filters(dialog.get(), G_LIST_MODEL(filters.get()));
    gtk_file_dialog_set_default_filter(dialog.get(), audio.get());

    // The callback owns a reference to the dialog for as long as it is up; the
    // window pointer is safe because closing the window destroys the dialog.
    gtk_file_dialog_open_multiple(
        dialog.release(), window_, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* self = static_cast<MainWindow*>(data);
            auto  owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
            GErrorPtr error;
            auto files = GObjectPtr<GListModel>::adopt(
                gtk_file_dialog_open_multiple_finish(owned.get(), result, &error.value));
            if (files) {
                self->openUrls(urlsFrom(files.get()));
            }
        },
        this);
}

void MainWindow::openFolder() {
    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
    gtk_file_dialog_set_title(dialog.get(), app::tr("Open Folder").c_str());
    gtk_file_dialog_select_folder(
        dialog.release(), window_, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* self  = static_cast<MainWindow*>(data);
            auto  owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
            GErrorPtr error;
            auto folder = GObjectPtr<GFile>::adopt(
                gtk_file_dialog_select_folder_finish(owned.get(), result, &error.value));
            if (folder) {
                GStr path(g_file_get_path(folder.get()));
                if (path) {
                    self->openUrls({Url::fromLocalPath(std::filesystem::path{path.c_str()})});
                }
            }
        },
        this);
}

void MainWindow::showPreferences(PreferencesPage page) {
    if (!preferences_) {
        preferences_ = std::make_unique<PreferencesDialog>(session_, presence_->hasTray());
        preferencesChanged_ = preferences_->settingChanged.connect(
            [this](const std::string& key) { session_.settingChanged(key); });
        // Dropped once it closes -- from an idle, not from inside the closed
        // signal, where the handler being run is one the dialog's own
        // connections would sever on the way out.
        preferences_->closed.connect([this] {
            postToMainContext([this] {
                preferencesChanged_.reset();
                preferences_.reset();
            });
        }).release();
    }
    preferences_->present(GTK_WIDGET(window_), page);
}

void MainWindow::setMiniMode(bool mini) {
    // Recorded as it changes, which is where Cog records it (AppController.m:
    // 1027, in -setMiniMode: itself) rather than on the way out: after a
    // crash, the mode you were last in is the one you come back to.
    session_.settings().setMiniMode(mini);

    if (mini) {
        if (!mini_) {
            mini_ = std::make_unique<MiniWindow>(
                ADW_APPLICATION(gtk_window_get_application(window_)), G_ACTION_GROUP(window_));
            subscriptions_.push_back(mini_->dismissed.connect([this] { setMiniMode(false); }));
            subscriptions_.push_back(mini_->seekRequested.connect(
                [this](double seconds) { session_.playback().seek(seconds); }));
            subscriptions_.push_back(mini_->volumeChanged.connect([this](double gain) {
                session_.settings().setVolume(gain);
                session_.setVolume(gain);
                settingScales_ = true;
                gtk_adjustment_set_value(volumeAdjustment_, gain * 100.0);
                settingScales_ = false;
            }));
        }
        // A fresh window read nothing; the shape it has to be handed, or it
        // opens mid-track with a plain bar until the next one.
        mini_->setWaveformStyle(SeekBar::styleFrom(session_.settings()));
        mini_->setWaveformMode(session_.settings().WaveformSeekBar());
        mini_->setWaveform(seekBar_->waveform());
        const PlaylistEntry* entry = session_.playlist().find(session_.currentTrack());
        mini_->setNowPlaying(entry != nullptr ? entry->title() : std::string{},
                             entry != nullptr ? entry->artist.str() : std::string{});
        mini_->setVolume(session_.settings().Volume());
        mini_->setPlaybackState(session_.playback().playing(), session_.playback().paused());
        mini_->present();
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
        actions_->setChecked(CommandId::ViewMiniPlayer, true);
        return;
    }

    if (mini_) {
        mini_->hide();
    }
    gtk_window_unminimize(window_);
    gtk_window_present(window_);
    actions_->setChecked(CommandId::ViewMiniPlayer, false);
}

void MainWindow::showAbout() { showAboutDialog(GTK_WIDGET(window_), session_.registry()); }

void MainWindow::showFileTree(bool show) {
    adw_overlay_split_view_set_show_sidebar(splitView_, show ? TRUE : FALSE);
    actions_->setChecked(CommandId::ViewFileTree, show);
}

void MainWindow::togglePanel(const char* page) {
    const bool        shown   = adw_overlay_split_view_get_show_sidebar(panelsView_);
    const char* const current = adw_view_stack_get_visible_child_name(panelsStack_);
    if (shown && g_strcmp0(current, page) == 0) {
        adw_overlay_split_view_set_show_sidebar(panelsView_, FALSE);
        return;
    }
    adw_view_stack_set_visible_child_name(panelsStack_, page);
    adw_overlay_split_view_set_show_sidebar(panelsView_, TRUE);
    refreshPaneState();
    refreshPanels();
}

void MainWindow::showTool(const std::string& name, bool show) {
    tools_->setShown(name, show);
    gtk_widget_set_visible(GTK_WIDGET(toolsBin_), tools_->anyShown());
    refreshPaneState();
    refreshVisualizers();
}

void MainWindow::refreshVisualizers() {
    const bool playing = session_.playback().playing() && !session_.playback().paused();
    spectrum_->setActive(tools_->shown("spectrum") && playing);
    scope_->setActive(tools_->shown("scope") && playing);
#ifdef XPCOG_HAVE_SC55_PANEL
    // Shown is enough: the panel explains itself when nothing is playing on
    // a machine that has one, which is a thing worth seeing.
    sc55_->setActive(tools_->shown("sc55"));
#endif
}

void MainWindow::applyWaveformSetting() {
    const Settings&            settings = session_.settings();
    const SeekBar::WaveformStyle style  = SeekBar::styleFrom(settings);
    const bool                 on       = settings.WaveformSeekBar();
    seekBar_->setWaveformStyle(style);
    seekBar_->setWaveformMode(on);
    if (mini_) {
        mini_->setWaveformStyle(style);
        mini_->setWaveformMode(on);
    }
}

TrackId MainWindow::panelTrackId() const {
    if (session_.settings().PanelFollowMode() == 1) {
        return session_.currentTrack();
    }
    const std::vector<TrackId> selection = playlist_->selectedTracks();
    return selection.empty() ? session_.currentTrack() : selection.front();
}

void MainWindow::refreshPanels() {
    // Cheap while the sidebar is hidden, which is most of the time -- and it
    // matters, because metadata arriving during a scan would otherwise redraw
    // twenty fields per file.
    if (!adw_overlay_split_view_get_show_sidebar(panelsView_)) {
        return;
    }
    const TrackId        id      = panelTrackId();
    const PlaylistEntry* entry   = session_.playlist().find(id);
    const char* const    current = adw_view_stack_get_visible_child_name(panelsStack_);
    if (g_strcmp0(current, "lyrics") == 0) {
        // Timed lyrics are followed only for the track that is playing; for
        // any other the position is some other song's.
        lyrics_->setTimed(session_.settings().LyricsSynced());
        lyrics_->showEntry(entry, id != kInvalidTrackId && id == session_.currentTrack());
    } else {
        info_->showEntry(entry);
    }
}

void MainWindow::refreshPaneState() {
    const bool        shown   = adw_overlay_split_view_get_show_sidebar(panelsView_);
    const char* const current = adw_view_stack_get_visible_child_name(panelsStack_);
    actions_->setChecked(CommandId::ViewInfo, shown && g_strcmp0(current, "info") == 0);
    actions_->setChecked(CommandId::ViewLyrics, shown && g_strcmp0(current, "lyrics") == 0);
    actions_->setChecked(CommandId::ViewSpectrum, tools_->shown("spectrum"));
    actions_->setChecked(CommandId::ViewOscilloscope, tools_->shown("scope"));
    actions_->setChecked(CommandId::ViewEqualizer, tools_->shown("equalizer"));
    actions_->setChecked(CommandId::ViewSpeed, tools_->shown("speed"));
    actions_->setChecked(CommandId::ViewSc55Panel, tools_->shown("sc55"));
}

void MainWindow::showPlaylistMenu(double x, double y) {
    const GdkRectangle at = {static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(playlistMenu_), &at);
    gtk_popover_popup(GTK_POPOVER(playlistMenu_));
}

void MainWindow::trashSelected() {
    std::vector<TrackId> ids;
    for (const TrackId id : playlist_->selectedTracks()) {
        const PlaylistEntry* entry = session_.playlist().find(id);
        if (entry != nullptr && entry->url.localPath().has_value()) {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return;
    }

    Settings& settings = session_.settings();
    if (settings.TrashAskedConsent()) {
        session_.trashTracks(ids);
        return;
    }

    // Cog's question, with its third button as a check: "yes, and stop
    // asking". A plain yes trashes these files and leaves the question.
    const std::string heading =
        app::fmt(app::trn("Move %u file to the trash?", "Move %u files to the trash?", ids.size()),
                 static_cast<unsigned>(ids.size()));
    AdwDialog* dialog = adw_alert_dialog_new(
        heading.c_str(),
        app::tr("Undo puts the rows back in the playlist. It does not bring the files back -- "
                "restore those from the trash itself.")
            .c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", app::tr("Cancel").c_str(),
                                   "trash", app::tr("Move to Trash").c_str(), nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "trash",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    GtkWidget* again = gtk_check_button_new_with_label(app::tr("Do not ask again").c_str());
    gtk_widget_set_halign(again, GTK_ALIGN_CENTER);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), again);

    struct Pending {
        MainWindow*          self;
        std::vector<TrackId> ids;
        GtkWidget*           again;
    };
    auto* pending = new Pending{this, std::move(ids), again};
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window_), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Pending> p(static_cast<Pending*>(data));
            const char* response =
                adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (g_strcmp0(response, "trash") != 0) {
                return;
            }
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(p->again))) {
                p->self->session_.settings().setTrashAskedConsent(true);
            }
            p->self->session_.trashTracks(p->ids);
        },
        pending);
}

void MainWindow::askCrashReportingConsent() {
    Settings& settings = session_.settings();
    if (!platform::crashReportingAvailable() || settings.SentryAskedConsent()) {
        return;
    }

    // Recorded *before* the answer, which is what Cog does and is not an
    // oversight in either place (Window/MainWindow.m:36 writes the flag outside
    // the completion handler). The promise is "we won't ask you again", and it
    // has to hold for the person who closed the dialog without answering just
    // as much as for the one who pressed No -- otherwise declining to decide is
    // the one response that gets asked again every launch.
    settings.setSentryAskedConsent(true);

    AdwDialog* dialog = adw_alert_dialog_new(
        app::tr("Crash reporting").c_str(),
        app::tr("Would you like to allow Sentry to submit crash reports?\n\n"
                "You may turn this off again in Preferences. We won't ask you again.")
            .c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "no", app::tr("No").c_str(), "yes",
                                   app::tr("Yes").c_str(), nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "yes",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "no");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "no");

    GtkWidget* policy = gtk_link_button_new_with_label(std::string{platform::kPrivacyPolicyUrl}.c_str(),
                                                       app::tr("Privacy policy").c_str());
    gtk_widget_set_halign(policy, GTK_ALIGN_CENTER);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), policy);

    // Whichever window is actually on screen. Cog has two prompts for this,
    // one per window class; here there is one, and it asks which mode it is in.
    GtkWidget* parent = (mini_ && mini_->visible()) ? mini_->widget() : GTK_WIDGET(window_);
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), parent, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto*       self     = static_cast<MainWindow*>(data);
            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            Settings&   stored   = self->session_.settings();
            if (g_strcmp0(response, "yes") != 0) {
                // No is already the stored default; writing it anyway so that
                // the answer is a value someone can see rather than an absence
                // they have to infer.
                stored.setSentryConsented(false);
                stored.sync();
                return;
            }
            stored.setSentryConsented(true);
            // Flushed here rather than at quit: this is the one setting whose
            // whole point is to be read on the *next* launch, including the
            // launch after a crash, and a crash is precisely the exit that
            // never reaches Settings::sync.
            stored.sync();
            platform::startCrashReporting();
        },
        this);
}

void MainWindow::persistState() {
    // Wayland cannot position a window and GTK 4 cannot ask where one is, so
    // the size and whether it is maximised are all there is to remember --
    // under this frontend's own key, because the wx one holds a position too
    // and writing zeros there would move that window to a corner.
    int width  = 0;
    int height = 0;
    gtk_window_get_default_size(window_, &width, &height);
    if (width > 0 && height > 0) {
        session_.settings().setRawValue("xpcog.gtk.window.geometry",
                                        std::to_string(width) + "," + std::to_string(height) + "," +
                                            (gtk_window_is_maximized(window_) ? "1" : "0"));
    }
    playlist_->persistColumnWidths();

    // Which panes are open, which page the right sidebar shows and how tall
    // the strip is, under a key of this frontend's own: the wx layout is a
    // wxAUI perspective, which describes a different arrangement.
    std::string panes;
    for (const char* name : {"spectrum", "scope", "equalizer", "speed", "sc55"}) {
        panes += std::string(name) + "=" + (tools_->shown(name) ? "1" : "0") + ";";
    }
    panes += std::string("panels=") +
             (adw_overlay_split_view_get_show_sidebar(panelsView_) ? "1" : "0") + ";";
    if (const char* page = adw_view_stack_get_visible_child_name(panelsStack_)) {
        panes += std::string("page=") + page + ";";
    }
    panes += "strip=" + std::to_string(gtk_paned_get_position(toolsPaned_)) + ";";
    session_.settings().setRawValue("xpcog.gtk.window.panes", panes);

    // The two keys the wx frontend keeps too, in its spelling, so a root
    // chosen in one is the root in the other.
    session_.settings().setRawValue("xpcog.fileTree.root", fileTree_->rootPath());
    session_.settings().setRawValue("xpcog.window.fileTree",
                                    adw_overlay_split_view_get_show_sidebar(splitView_) ? "1" : "0");
}

void MainWindow::restoreState() {
    const std::string geometry = session_.settings().rawValue("xpcog.gtk.window.geometry");
    int               width     = 0;
    int               height    = 0;
    int               maximised = 0;
    if (std::sscanf(geometry.c_str(), "%d,%d,%d", &width, &height, &maximised) == 3 && width > 0 &&
        height > 0) {
        gtk_window_set_default_size(window_, width, height);
        if (maximised != 0) {
            gtk_window_maximize(window_);
        }
    }

    const std::string root = session_.settings().rawValue("xpcog.fileTree.root");
    if (!root.empty()) {
        fileTree_->setRootPath(root);
    } else if (const char* music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC)) {
        // A first launch: the desktop's music folder, which is what a folder
        // browser in a music player is for, rather than the whole filesystem.
        fileTree_->setRootPath(music);
    } else {
        fileTree_->setRootPath(g_get_home_dir());
    }
    // Absent means closed, which is what a first launch gets. Only an explicit
    // "1" opens it, as in the wx frontend.
    showFileTree(session_.settings().rawValue("xpcog.window.fileTree") == "1");

    // The panes. Absent means the first-launch layout: nothing open.
    const std::string panes = session_.settings().rawValue("xpcog.gtk.window.panes");
    std::string_view  rest  = panes;
    while (!rest.empty()) {
        const std::size_t      semicolon = rest.find(';');
        const std::string_view entry     = rest.substr(0, semicolon);
        rest = semicolon == std::string_view::npos ? std::string_view{} : rest.substr(semicolon + 1);
        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string key(entry.substr(0, equals));
        const std::string value(entry.substr(equals + 1));
        if (key == "panels") {
            adw_overlay_split_view_set_show_sidebar(panelsView_, value == "1");
        } else if (key == "page") {
            adw_view_stack_set_visible_child_name(panelsStack_, value.c_str());
        } else if (key == "strip") {
            if (const int position = std::atoi(value.c_str()); position > 0) {
                gtk_paned_set_position(toolsPaned_, position);
            }
        } else {
            tools_->setShown(key, value == "1");
        }
    }
    gtk_widget_set_visible(GTK_WIDGET(toolsBin_), tools_->anyShown());
    refreshPaneState();
    refreshVisualizers();

    // The mini player, if that is where the listener left off. Cog restores
    // it at launch from the same key (AppController.m:314). The application
    // presents "the window" right after building this; present() knows which
    // of the two that is.
    if (session_.settings().MiniMode()) {
        setMiniMode(true);
    }
}

// --- small things ------------------------------------------------------------------

void MainWindow::quit() {
    // Not gtk_window_close: a dialog presented over the window -- Preferences,
    // Open URL -- takes the close request for itself, and Quit would then do
    // nothing visible. Save as the close handler would, and end the process.
    if (quitting_) {
        return;
    }
    quitting_ = true;
    persistState();
    session_.save();
    g_application_quit(G_APPLICATION(gtk_window_get_application(window_)));
}

void MainWindow::present() {
    // Whichever window the mode says is the player's: the mini player is a
    // mode, and raising "the window" while in it means raising that one.
    if (mini_ && mini_->visible()) {
        mini_->present();
        return;
    }
    gtk_window_unminimize(window_);
    gtk_window_present(window_);
}

void MainWindow::openUrls(const std::vector<Url>& urls) { session_.addUrls(urls); }

void MainWindow::setStatusText(const std::string& text) {
    gtk_label_set_text(statusLabel_, text.c_str());
}

void MainWindow::setClock(double seconds, double duration) {
    const std::string text = app::formatClock(seconds) + " / " + app::formatClock(duration);
    gtk_label_set_text(clockLabel_, text.c_str());
}

}  // namespace xpcog::gtk
