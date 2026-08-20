#include "MainFrame.hpp"

#include "AboutDialog.hpp"
#include "AppIcon.hpp"
#include "Commands.hpp"
#include "EqualizerPanel.hpp"
#include "FileTree.hpp"
#include "InfoPanel.hpp"
#include "MiniFrame.hpp"
#include "OpenUrlDialog.hpp"
#include "PreferencesDialog.hpp"
#include "Sc55Panel.hpp"
#include "SpectrumPanel.hpp"
#include "LucideIcon.hpp"
#include "PlaylistDataModel.hpp"
#include "SeekBar.hpp"
#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"
#include "xpcog/core/library/PlaylistFile.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <wx/bmpbuttn.h>
#include <wx/dataview.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/gauge.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <utility>

namespace xpcog::app {
namespace {

using Column = PlaylistView::Column;

enum : int {
    kListId = FirstWidgetId + 40,
    kSeekBarId,
    kVolumeId,
    kFilterId,
    kScanCancelId,
};

[[nodiscard]] std::string formatClock(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const auto total   = static_cast<int>(seconds + 0.5);
    const int  minutes = total / 60;
    const int  rest    = total % 60;
    const std::string padded = rest < 10 ? "0" + std::to_string(rest) : std::to_string(rest);
    if (minutes >= 60) {
        const int hours = minutes / 60;
        const int mins  = minutes % 60;
        return std::to_string(hours) + ":" + (mins < 10 ? "0" : "") +
               std::to_string(mins) + ":" + padded;
    }
    return std::to_string(minutes) + ":" + padded;
}

/// Files dropped from the file manager onto the window.
class PlaylistDropTarget : public wxFileDropTarget {
public:
    explicit PlaylistDropTarget(std::function<void(std::vector<Url>)> onDrop)
        : onDrop_(std::move(onDrop)) {}

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override {
        std::vector<Url> urls;
        urls.reserve(filenames.GetCount());
        for (const wxString& name : filenames) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path{name.ToStdWstring()}));
        }
        if (urls.empty()) {
            return false;
        }
        onDrop_(std::move(urls));
        return true;
    }

private:
    std::function<void(std::vector<Url>)> onDrop_;
};

}  // namespace

// --- construction -------------------------------------------------------

MainFrame::MainFrame(const PluginRegistry& registry, Settings& settings,
                     Dispatcher dispatch)
    : wxFrame(nullptr, wxID_ANY, "XPCog", wxDefaultPosition, wxSize(1100, 680)),
      registry_(registry),
      settings_(settings),
      dispatch_(std::move(dispatch)),
      view_(playlist_) {
    SetIcons(applicationIcons());

    playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
    playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
    playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());

    library_ = std::make_unique<Library>();
    if (!library_->open(platform::libraryDatabasePath())) {
        // A library that will not open is not fatal: the player still plays, it
        // just will not remember the playlist. Saying so once beats failing to
        // launch.
        setStatusText("Library unavailable: " + library_->lastError());
        library_.reset();
    }

    playback_ =
        std::make_unique<PlaybackController>(registry_, playlist_, settings_, dispatch_);

    SetMenuBar(buildMenuBar());
    buildUi();

    // Both of these want the native window handle, and neither can be built
    // before there is one. Under wx the frame's handle exists as soon as it does,
    // which is why the media integration no longer has to go looking for a window
    // and retry until it finds one.
    void* const handle = GetHandle();
    media_             = platform::MediaIntegration::create(dispatch_, handle);
    taskbar_           = platform::TaskbarIntegration::create(handle);

    presence_ = std::make_unique<StatusPresence>(this);

    wireUp();
    restoreState();

    if (library_ && library_->loadPlaylist(playlist_)) {
        setStatusText(statusSummary());
    }
    // Restoring the saved playlist is not an edit the user made, so it must not
    // be the first thing Undo offers to take back.
    undo_.clear();
}

MainFrame::~MainFrame() {
    // Not optional, and not something the destructor ordering can substitute for:
    // the manager holds pointers to windows that are about to be destroyed with
    // the frame, and UnInit() is what detaches it from them first.
    auiManager_.UnInit();

    // The scan borrows the registry and the PluginCache, and the cache is a
    // member of this window, so the task has to go first -- otherwise its thread
    // outlives what it is reading from. ~ScanTask cancels and joins.
    scan_.reset();
}

void MainFrame::buildUi() {
    auiManager_.SetManagedWindow(this);

    // The playlist and the file browser are the centre, and the browser is a
    // splitter pane rather than a dock: Cog's file tree is a fixed part of the
    // window and behaves as one, and the View menu toggles the split.
    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_3DSASH);
    splitter_->SetMinimumPaneSize(FromDIP(140));

    tree_ = new FileTree(splitter_, registry_);

    list_ = new wxDataViewCtrl(splitter_, kListId, wxDefaultPosition, wxDefaultSize,
                               wxDV_MULTIPLE | wxDV_ROW_LINES);
    model_ = new PlaylistDataModel(view_);
    list_->AssociateModel(model_);
    // AssociateModel takes a reference of its own; without this the model leaks,
    // because it starts life with one already.
    model_->DecRef();
    model_->appendColumnsTo(list_);

    splitter_->SplitVertically(tree_, list_, FromDIP(260));

    auto* transport = new wxPanel(this, wxID_ANY);
    buildTransport(transport);

    // The optional panes, in the places the Qt build docked them: the wide, short
    // ones along the bottom and the tall column of fields at the right.
    equalizer_ = new EqualizerPanel(this, settings_);
    info_      = new InfoPanel(this, library_.get());
    spectrum_  = new SpectrumPanel(this, playback_->tap());
    spectrum_->applySettings(settings_);

#ifdef XPCOG_HAVE_SC55_PANEL
    sc55_ = new Sc55Panel(this, [this] { return playback_->position(); });
#endif

    // Every pane is named, and the names are what a saved perspective refers to.
    // Renaming one silently discards that pane's saved position, so these are as
    // load-bearing as the object names QMainWindow::saveState() needed.
    auiManager_.AddPane(splitter_, wxAuiPaneInfo().Name("playlist").CenterPane());

    // The transport is a pane so the manager owns the whole frame, but it is not
    // a *dockable* one. Hiding the only play button leaves a window with no way to
    // start playback and no obvious way back, which is why the Qt build removed
    // the transport from its own context menu too.
    // Resizable, and that is the fix rather than an oversight corrected.
    //
    // Resizable(false) is Fixed(), and framemanager.cpp sets a fixed pane's
    // proportion to 0 -- so it gets exactly its best size inside the dock and the
    // rest of the dock stays empty. That is why the transport sat at its natural
    // width with a gap beside it instead of spanning the window.
    //
    // What was actually wanted is fixed *vertically* and stretched horizontally,
    // which is two different flags. DockFixed keeps the dock's thickness -- its
    // height, here -- from being dragged, while the pane keeps a proportion and
    // fills the width.
    //
    // MaxSize is deliberately not used for this: wxAUI reads it only for floating
    // panes and when saving a perspective, and it has no effect at all on a
    // docked one. Reaching for it first was the wrong guess.
    const int transportHeight = transport->GetBestSize().GetHeight();
    auiManager_.AddPane(transport, wxAuiPaneInfo()
                                       .Name("transport")
                                       .Top()
                                       .CaptionVisible(false)
                                       .CloseButton(false)
                                       .Dockable(false)
                                       .Floatable(false)
                                       .Movable(false)
                                       .DockFixed(true)
                                       .Layer(10)
                                       .MinSize(FromDIP(320), transportHeight)
                                       .BestSize(FromDIP(900), transportHeight));

    auiManager_.AddPane(spectrum_, wxAuiPaneInfo()
                                       .Name("spectrum")
                                       .Caption("Spectrum")
                                       .Bottom()
                                       .BestSize(FromDIP(wxSize(400, 140)))
                                       .MinSize(FromDIP(wxSize(120, 60)))
                                       .Show());

    // Hidden rather than absent, so it keeps a place in the layout to come back
    // to. 31 sliders is a lot of window to open on someone who wanted a music
    // player.
    // Sized from the panel rather than guessed at. Thirty-two columns have a
    // real height -- readout, slider, label, and the footer under them -- and a
    // pane shorter than that clips the labels off the bottom, where there is no
    // vertical scrolling to reach them. The width is asked for generously and
    // scrolls horizontally when it cannot be had.
    const wxSize equalizerBest = equalizer_->GetBestSize();
    auiManager_.AddPane(equalizer_, wxAuiPaneInfo()
                                        .Name("equalizer")
                                        .Caption("Equalizer")
                                        .Bottom()
                                        .BestSize(equalizerBest)
                                        .MinSize(FromDIP(240), equalizerBest.GetHeight())
                                        .Hide());

    auiManager_.AddPane(info_, wxAuiPaneInfo()
                                   .Name("info")
                                   .Caption("Info")
                                   .Right()
                                   .BestSize(FromDIP(wxSize(300, 400)))
                                   .MinSize(FromDIP(wxSize(220, 200)))
                                   .Hide());

#ifdef XPCOG_HAVE_SC55_PANEL
    auiManager_.AddPane(sc55_, wxAuiPaneInfo()
                                   .Name("sc55")
                                   .Caption("SC-55 Panel")
                                   .Bottom()
                                   .BestSize(FromDIP(wxSize(420, 200)))
                                   .MinSize(FromDIP(wxSize(200, 100)))
                                   .Hide());
#endif

    auiManager_.Update();

    // Two fields: the summary, and the now-playing text with the scan widgets
    // positioned over it.
    CreateStatusBar(2);

    scanBar_ = new wxGauge(GetStatusBar(), wxID_ANY, 100, wxDefaultPosition,
                           FromDIP(wxSize(140, 14)));
    scanBar_->Hide();

    // Losing the modal progress dialog the Qt build started with also loses its
    // Cancel button, and a scan of a mistakenly-dropped drive needs a way out
    // that is not quitting.
    scanCancel_ = new wxBitmapButton(GetStatusBar(), kScanCancelId, lucideIcon("x"),
                                     wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    scanCancel_->SetToolTip("Stop reading files");
    scanCancel_->Hide();

    // wxStatusBar has no addPermanentWidget, so its children are positioned by
    // hand against the field rectangle. This is the wx sample's own technique.
    GetStatusBar()->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        wxRect field;
        if (!GetStatusBar()->GetFieldRect(1, field)) {
            return;
        }
        const int gap = FromDIP(4);
        const wxSize cancel = scanCancel_->GetSize();
        const wxSize bar    = scanBar_->GetSize();
        scanCancel_->Move(field.GetRight() - cancel.GetWidth() - gap,
                          field.GetY() + ((field.GetHeight() - cancel.GetHeight()) / 2));
        scanBar_->Move(field.GetRight() - cancel.GetWidth() - bar.GetWidth() - (2 * gap),
                       field.GetY() + ((field.GetHeight() - bar.GetHeight()) / 2));
    });

    SetDropTarget(new PlaylistDropTarget(
        [this](std::vector<Url> urls) { addUrls(urls, -1); }));
}

void MainFrame::buildTransport(wxWindow* parent) {
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    for (const CommandId id : transportLayout()) {
        auto* button = new wxBitmapButton(parent, id, lucideIcon(commandIcon(id)),
                                          wxDefaultPosition, wxDefaultSize,
                                          wxBORDER_NONE);
        button->SetBitmapDisabled(lucideIconDisabled(commandIcon(id)));
        sizer->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(2));
        transportButtons_.push_back(button);
        if (id == PlaybackPlayPause) {
            playPauseButton_ = button;
        }
    }

    seekBar_ = new SeekBar(parent, kSeekBarId);
    sizer->Add(seekBar_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));

    clock_ = new wxStaticText(parent, wxID_ANY, "0:00 / 0:00", wxDefaultPosition,
                              FromDIP(wxSize(90, -1)), wxALIGN_CENTRE_HORIZONTAL);
    sizer->Add(clock_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    volume_ = new wxSlider(parent, kVolumeId,
                           static_cast<int>(settings_.Volume() * 100.0), 0, 100,
                           wxDefaultPosition, FromDIP(wxSize(110, -1)));
    volume_->SetToolTip("Volume");
    sizer->Add(volume_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    filter_ = new wxSearchCtrl(parent, kFilterId, wxEmptyString, wxDefaultPosition,
                               FromDIP(wxSize(200, -1)));
    filter_->ShowCancelButton(true);
    filter_->SetDescriptiveText("Filter");
    sizer->Add(filter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    parent->SetSizer(sizer);
}

// --- wiring -------------------------------------------------------------

void MainFrame::wireUp() {
    const auto observe = [this](auto& signal, auto handler) {
        subscriptions_.push_back(signal.connect(std::move(handler)));
    };

    // --- playback -------------------------------------------------------
    observe(playback_->positionChanged,
            [this](double seconds, double duration) { onPositionChanged(seconds, duration); });
    observe(playback_->currentTrackChanged, [this](TrackId id) { onCurrentTrackChanged(id); });
    observe(playback_->playbackStateChanged,
            [this](bool playing, bool paused) { onPlaybackStateChanged(playing, paused); });
    observe(playback_->startPending, [this](TrackId id) {
        const PlaylistEntry* entry = playlist_.find(id);
        setStatusText(entry != nullptr ? "Connecting to " + entry->title() + "..."
                                       : std::string{"Connecting..."});
    });
    observe(playback_->playbackFailed,
            [this](TrackId, const std::string& reason) { setStatusText(reason); });
    observe(playback_->trackMetadataChanged, [this](TrackId id) {
        // A stream renamed itself. The row redraws from the view's own
        // notification; what has to happen here is the title bar, the status
        // line and the OS's card, all of which read the entry rather than the
        // change.
        if (id == currentTrack_) {
            onCurrentTrackChanged(id);
        }
    });

    // --- the media keys and the OS's now-playing widget ------------------
    //
    // They drive the same commands the buttons do, rather than reaching into the
    // engine separately.
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

    // MPRIS only, on Linux. The other two platforms never publish these, so there
    // is nothing to guard: a signal that is never sent costs a connection.
    observe(media_->raiseRequested, [this] {
        Iconize(false);
        Show();
        Raise();
    });
    observe(media_->quitRequested, [this] { Close(true); });
    observe(media_->volumeRequested, [this](float gain) {
        // Through the slider rather than straight to the engine, so the panel and
        // the window cannot end up showing different volumes.
        volume_->SetValue(static_cast<int>(std::lround(gain * 100.0F)));
        playback_->setVolume(gain);
    });
    observe(media_->openUrlRequested, [this](const Url& url) { openUrls({url}); });

    // --- the file browser ------------------------------------------------
    observe(tree_->activated, [this](const std::vector<Url>& urls) { addUrls(urls); });
    observe(tree_->addRequested, [this](const std::vector<Url>& urls) { addUrls(urls); });

    // --- the seek bar ----------------------------------------------------
    observe(seekBar_->seekRequested, [this](double seconds) { playback_->seek(seconds); });
    observe(seekBar_->scrubbed, [this](double seconds) {
        clock_->SetLabelText(toWx(formatClock(seconds) + " / " + formatClock(duration_)));
    });

    // --- the spectrum ----------------------------------------------------
    //
    // The sample rate is what its band table is built against, and it is not
    // known until a device has been negotiated -- which happens when a track
    // starts, not when the panel is created.
    observe(playback_->playbackStateChanged, [this](bool playing, bool paused) {
        spectrum_->setSampleRate(playback_->sampleRate());
        spectrum_->setActive(paneShown(spectrum_) && playing && !paused);
    });

    // --- the equaliser ---------------------------------------------------
    observe(equalizer_->settingChanged,
            [this](const std::string& key) { onSettingChanged(key); });

    // --- the playlist selection ------------------------------------------
    //
    // Cog's rule: the info panel follows the selection when there is one, and the
    // playing track otherwise.
    list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                [this](wxDataViewEvent&) { refreshInfo(); });

    // --- the undo stack --------------------------------------------------
    //
    // Only the status line: the menu labels come from EVT_UPDATE_UI, which asks
    // the stack directly every idle and therefore cannot fall behind it.
    observe(undo_.changed, [this] { setStatusText(statusSummary()); });

    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& event) {
        activateRow(model_->GetRow(event.GetItem()));
    });

    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK, [this](wxDataViewEvent& event) {
        const auto column = static_cast<Column>(event.GetColumn());
        // Three states rather than two, and the third is the point: ascending,
        // descending, then back to playlist order. A sort you cannot get out of
        // is a playlist whose real order you can no longer see.
        if (view_.sortColumn() != column) {
            view_.setSort(column, true);
        } else if (view_.sortAscending()) {
            view_.setSort(column, false);
        } else {
            view_.setSort(PlaylistView::kNoSort, true);
        }
    });

    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
        view_.setFilter(toUtf8(event.GetString()));
    });
    filter_->Bind(wxEVT_SEARCH_CANCEL, [this](wxCommandEvent&) {
        filter_->Clear();
        view_.setFilter({});
    });

    volume_->Bind(wxEVT_SLIDER, [this](wxCommandEvent& event) {
        const double gain = event.GetInt() / 100.0;
        playback_->setVolume(gain);
        // MPRIS publishes the volume as a property a desktop environment both
        // reads and writes, so the value has to be pushed or the panel's slider
        // sits wherever it last put it while the audio does something else.
        media_->setVolume(static_cast<float>(gain));
    });

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Everything not yet started goes too: cancelling one folder of a dropped
        // batch and then watching the next one start is not what the button looks
        // like it does.
        pendingScans_.clear();
        if (scan_) {
            scan_->cancel();
        }
    }, kScanCancelId);

    // The system appearance changed. Every Lucide glyph is stroked in a colour
    // read at the moment it was built, so without this a switch to dark mode
    // leaves a toolbar of black on near-black.
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
        event.Skip();
        forgetLucideIcons();
        refreshTransportIcons();
        scanCancel_->SetBitmap(lucideIcon("x"));
        tree_->refreshIcons();
        if (mini_ != nullptr) {
            mini_->refreshIcons();
        }
    });

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        // Layout and playlist are saved whichever way this goes, and *before* the
        // decision below: the state is the same either way, and saving only on a
        // real quit means a session that ends in the tray loses everything.
        persistState();

        // Close to tray, where there is a tray and the listener asked for it.
        // hasTrayIcon() rather than "is there any presence": on macOS the Dock
        // menu exists while a tray icon does not, and hiding there would leave
        // nothing to click.
        if (!quitting_ && settings_.CloseToTray() && presence_->hasTrayIcon() &&
            event.CanVeto()) {
            event.Veto();
            Hide();
            if (!trayHintShown_) {
                trayHintShown_ = true;
                presence_->notify("XPCog is still running",
                                  "Playback continues. Use the tray icon to bring "
                                  "the window back or to quit.");
            }
            return;
        }

        // The mini player is a child frame and vetoes its own close, so it has to
        // be destroyed explicitly or the application never exits.
        if (mini_ != nullptr) {
            mini_->Destroy();
            mini_ = nullptr;
        }
        // The tray icon holds a reference to this window; removing it first stops
        // a menu built during teardown from reaching a half-destroyed frame.
        presence_->RemoveIcon();
        event.Skip();
    });

    // Geometry, tracked as it changes. Both events, because a window can be
    // moved without being resized and the reverse.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        rememberGeometry();
    });
    Bind(wxEVT_MOVE, [this](wxMoveEvent& event) {
        event.Skip();
        rememberGeometry();
    });

    // A pane closed by its own button, rather than from the View menu. Nothing
    // has to be recorded -- EVT_UPDATE_UI reads the manager, so the menu's tick
    // follows on its own -- but the spectrum's clock is not the manager's to stop.
    Bind(wxEVT_AUI_PANE_CLOSE, [this](wxAuiManagerEvent& event) {
        event.Skip();
        if (event.GetPane() != nullptr && event.GetPane()->window == spectrum_) {
            spectrum_->setActive(false);
        }
    });

    bindCommands();
    bindUpdateUi();
}

void MainFrame::onSettingChanged(const std::string& key) {
    // The equaliser and the DSP chain are read by the engine when it is asked to,
    // so a band that moves has to say so or the slider does nothing until the
    // next track.
    if (key.starts_with("eq") || key == "enableFSurround" || key == "enableFading" ||
        key == "volumeScaling" || key == "enableHDCD") {
        playback_->reloadDsp();
        return;
    }

    // The device is read when the engine opens it, which is when a track starts.
    // Moving what is already playing is what reopenOutput() is for.
    if (key == "outputDeviceId" || key == "exclusiveOutput") {
        playback_->reopenOutput();
        return;
    }

    if (key == "floatingMiniWindow" && mini_ != nullptr) {
        mini_->setFloating(settings_.FloatingMiniWindow());
        return;
    }

    if (key.starts_with("spectrum")) {
        spectrum_->applySettings(settings_);
    }
}

void MainFrame::togglePane(wxWindow* pane, bool show) {
    if (pane == nullptr) {
        return;
    }
    wxAuiPaneInfo& info = auiManager_.GetPane(pane);
    if (!info.IsOk()) {
        return;
    }
    info.Show(show);
    // The manager, not Layout(). A pane's window is a child of the frame but its
    // *placement* is the manager's, so showing the window without this leaves it
    // sized zero and invisible.
    auiManager_.Update();
}

bool MainFrame::paneShown(wxWindow* pane) const {
    if (pane == nullptr) {
        return false;
    }
    // const_cast because wxAuiManager::GetPane has no const overload. Nothing is
    // mutated -- IsShown() is a read -- and the alternative is holding a second
    // copy of state the manager already owns, which is exactly the sort of
    // duplicate the Qt build's dock bookkeeping went wrong on.
    const wxAuiPaneInfo& info =
        const_cast<wxAuiManager&>(auiManager_).GetPane(pane);
    return info.IsOk() && info.IsShown();
}

void MainFrame::refreshInfo() {
    // Returns immediately while the panel is hidden, which is most of the time --
    // and matters, because metadata arriving during a scan would otherwise redraw
    // twenty fields per file.
    if (!paneShown(info_)) {
        return;
    }

    const std::vector<TrackId> selection = selectedTracks();
    const TrackId shown = selection.empty() ? currentTrack_ : selection.front();
    info_->showEntry(playlist_.find(shown));
}

void MainFrame::setMiniMode(bool mini) {
    if (mini) {
        if (mini_ == nullptr) {
            mini_ = new MiniFrame(this, *playback_, settings_);
            subscriptions_.push_back(
                mini_->dismissed.connect([this] { setMiniMode(false); }));
            subscriptions_.push_back(mini_->volumeChanged.connect([this](double gain) {
                volume_->SetValue(static_cast<int>(std::lround(gain * 100.0)));
            }));
        }
        mini_->refreshVolume();
        mini_->setNowPlaying(
            playlist_.find(currentTrack_) != nullptr ? playlist_.find(currentTrack_)->title()
                                                     : std::string{},
            playlist_.find(currentTrack_) != nullptr ? playlist_.find(currentTrack_)->artist
                                                     : std::string{});
        mini_->Show();
        mini_->Raise();
        Hide();
        return;
    }

    if (mini_ != nullptr) {
        mini_->Hide();
    }
    Show();
    Raise();
}

void MainFrame::openUrl() {
    OpenUrlDialog dialog(this, settings_);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }
    if (const std::optional<Url> url = Url::parse(dialog.url()); url.has_value()) {
        openUrls({*url});
    }
}

void MainFrame::showPreferences() {
    PreferencesDialog dialog(this, settings_);
    const Subscription subscription = dialog.settingChanged.connect(
        [this](const std::string& key) { onSettingChanged(key); });
    dialog.ShowModal();
}

void MainFrame::showAbout() {
    AboutDialog dialog(this, registry_);
    dialog.ShowModal();
}

void MainFrame::refreshTransportIcons() {
    for (std::size_t i = 0; i < transportButtons_.size(); ++i) {
        const CommandId id = transportLayout()[i];
        transportButtons_[i]->SetBitmap(lucideIcon(commandIcon(id)));
        transportButtons_[i]->SetBitmapDisabled(lucideIconDisabled(commandIcon(id)));
    }

    // Play/Pause carries whichever of the two the transport is asking for.
    // EVT_UPDATE_UI relabels the *menu* item from state every idle, but a
    // wxUpdateUIEvent can set a label and an enabled state and nothing else --
    // there is no bitmap on it -- so the button has to be told separately. That
    // is why the button kept showing a play triangle over a playing track.
    if (playPauseButton_ != nullptr) {
        const bool playing = playback_->playing() && !playback_->paused();
        const char* glyph  = playing ? "pause" : "play";
        playPauseButton_->SetBitmap(lucideIcon(glyph));
        playPauseButton_->SetBitmapDisabled(lucideIconDisabled(glyph));
        playPauseButton_->SetToolTip(playing ? "Pause" : "Play");
    }
}

void MainFrame::bindCommands() {
    // Both event types, for every command.
    //
    // A menu item and an accelerator raise wxEVT_MENU; a wxBitmapButton raises
    // wxEVT_BUTTON. They are different events carrying the same id, and binding
    // only the first is why the transport buttons did nothing at all while the
    // menu entries behind them worked -- a failure with no error attached to it,
    // because the event simply reached the end of the chain unhandled.
    //
    // Binding both here rather than translating at each button keeps the rule
    // that a command has one handler, whatever surface posts it.
    const auto on = [this](CommandId id, auto handler) {
        Bind(wxEVT_MENU, [handler](wxCommandEvent&) { handler(); }, id);
        Bind(wxEVT_BUTTON, [handler](wxCommandEvent&) { handler(); }, id);
    };

    on(FileOpen, [this] { openFiles(); });
    on(FileOpenFolder, [this] { openFolder(); });
    on(FileOpenUrl, [this] { openUrl(); });
    on(FileSavePlaylist, [this] { savePlaylistAs(); });
    on(FilePreferences, [this] { showPreferences(); });
    on(HelpAbout, [this] { showAbout(); });
    on(FileQuit, [this] {
        quitting_ = true;
        Close(true);
    });

    on(EditUndo, [this] { undo_.undo(); });
    on(EditRedo, [this] { undo_.redo(); });
    on(EditRemove, [this] { removeSelected(); });
    on(EditSelectAll, [this] { list_->SelectAll(); });
    on(EditRandomize, [this] {
        if (playlist_.size() > 1) {
            undo_.push(std::make_unique<RandomizeCommand>(playlist_, "Randomize"));
        }
    });

    on(PlaybackPlayPause, [this] { playback_->playPause(); });
    on(PlaybackStop, [this] { playback_->stop(); });
    on(PlaybackNext, [this] { playback_->next(); });
    on(PlaybackPrevious, [this] { playback_->previous(); });
    on(PlaybackEnqueue, [this] { enqueueSelected(); });

    on(ViewFileTreeRoot, [this] { tree_->chooseRootPath(); });
    on(ViewEqualizer, [this] { togglePane(equalizer_, !paneShown(equalizer_)); });
    on(ViewInfo, [this] {
        const bool showing = !paneShown(info_);
        togglePane(info_, showing);
        if (showing) {
            refreshInfo();
        }
    });
    on(ViewMiniPlayer, [this] { setMiniMode(mini_ == nullptr || !mini_->IsShown()); });
    on(ViewSpectrum, [this] {
        const bool showing = !paneShown(spectrum_);
        togglePane(spectrum_, showing);
        // The clock only runs while the pane is both visible and playing: a
        // 4096-point transform sixty times a second for a hidden widget is the
        // cost this guard exists to avoid.
        spectrum_->setActive(showing && playback_->playing() && !playback_->paused());
    });
#ifdef XPCOG_HAVE_SC55_PANEL
    on(ViewSc55Panel, [this] { togglePane(sc55_, !paneShown(sc55_)); });
#endif
    on(ViewFileTree, [this] {
        if (splitter_->IsSplit()) {
            splitter_->Unsplit(tree_);
        } else {
            splitter_->SplitVertically(tree_, list_, FromDIP(260));
        }
    });

    // The repeat and shuffle groups. Both write through to the settings, so the
    // choice survives a restart as Cog's does.
    const auto repeat = [this](RepeatMode mode) {
        playlist_.setRepeat(mode);
        settings_.setRepeatMode(static_cast<int>(mode));
    };
    on(OrderRepeatNone, [repeat] { repeat(RepeatMode::None); });
    on(OrderRepeatOne, [repeat] { repeat(RepeatMode::One); });
    on(OrderRepeatAlbum, [repeat] { repeat(RepeatMode::Album); });
    on(OrderRepeatAll, [repeat] { repeat(RepeatMode::All); });

    const auto shuffle = [this](ShuffleMode mode) {
        playlist_.setShuffle(mode);
        settings_.setShuffleMode(static_cast<int>(mode));
    };
    on(OrderShuffleOff, [shuffle] { shuffle(ShuffleMode::Off); });
    on(OrderShuffleAlbums, [shuffle] { shuffle(ShuffleMode::Albums); });
    on(OrderShuffleAll, [shuffle] { shuffle(ShuffleMode::All); });
}

void MainFrame::bindUpdateUi() {
    // What QAction's shared state used to do, and it does it better: every
    // surface carrying the id -- the menu bar, a context menu, the tray -- asks
    // the same handler, so none of them can drift.
    const auto update = [this](CommandId id, auto handler) {
        Bind(wxEVT_UPDATE_UI, [handler](wxUpdateUIEvent& event) { handler(event); }, id);
    };

    update(EditUndo, [this](wxUpdateUIEvent& event) {
        event.Enable(undo_.canUndo());
        // Relabelled from the stack every idle, which is what makes the old
        // refreshUndoActions() unnecessary rather than merely shorter.
        const std::string label = undo_.canUndo() ? "&Undo " + undo_.undoText() : "&Undo";
        event.SetText(toWx(label) + "\tCtrl+Z");
    });
    update(EditRedo, [this](wxUpdateUIEvent& event) {
        event.Enable(undo_.canRedo());
        const std::string label = undo_.canRedo() ? "&Redo " + undo_.redoText() : "&Redo";
        event.SetText(toWx(label) + "\tCtrl+Y");
    });

    update(EditRemove, [this](wxUpdateUIEvent& event) {
        event.Enable(list_->GetSelectedItemsCount() > 0);
    });
    update(PlaybackEnqueue, [this](wxUpdateUIEvent& event) {
        event.Enable(list_->GetSelectedItemsCount() > 0);
    });
    update(EditRandomize,
           [this](wxUpdateUIEvent& event) { event.Enable(playlist_.size() > 1); });
    update(EditSelectAll,
           [this](wxUpdateUIEvent& event) { event.Enable(view_.rowCount() > 0); });
    update(FileSavePlaylist,
           [this](wxUpdateUIEvent& event) { event.Enable(!playlist_.empty()); });

    update(PlaybackPlayPause, [this](wxUpdateUIEvent& event) {
        const bool playing = playback_->playing() && !playback_->paused();
        event.SetText(playing ? "&Pause" : "&Play");
        event.Enable(!playlist_.empty());
    });
    update(PlaybackStop,
           [this](wxUpdateUIEvent& event) { event.Enable(playback_->playing()); });
    update(PlaybackNext,
           [this](wxUpdateUIEvent& event) { event.Enable(!playlist_.empty()); });
    update(PlaybackPrevious,
           [this](wxUpdateUIEvent& event) { event.Enable(!playlist_.empty()); });

    update(ViewFileTree,
           [this](wxUpdateUIEvent& event) { event.Check(splitter_->IsSplit()); });
    update(ViewEqualizer,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(equalizer_)); });
    update(ViewInfo, [this](wxUpdateUIEvent& event) { event.Check(paneShown(info_)); });
    update(ViewMiniPlayer, [this](wxUpdateUIEvent& event) {
        event.Check(mini_ != nullptr && mini_->IsShown());
    });
    update(ViewSpectrum,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(spectrum_)); });
#ifdef XPCOG_HAVE_SC55_PANEL
    update(ViewSc55Panel,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(sc55_)); });
#else
    // Present even in a build without MIDI, where there is no emulator to render
    // a panel state and nothing that produces one. A command that appears and
    // disappears with a compile flag is worse than one that is occasionally
    // inert -- which is the same reasoning the Qt build's ActionRegistry gave.
    update(ViewSc55Panel, [](wxUpdateUIEvent& event) { event.Enable(false); });
#endif

    // The two exclusive groups read their state from the playlist rather than
    // being remembered here, so a change made anywhere shows up on the menu.
    const auto repeatIs = [this](RepeatMode mode) {
        return [this, mode](wxUpdateUIEvent& event) {
            event.Check(playlist_.repeat() == mode);
        };
    };
    update(OrderRepeatNone, repeatIs(RepeatMode::None));
    update(OrderRepeatOne, repeatIs(RepeatMode::One));
    update(OrderRepeatAlbum, repeatIs(RepeatMode::Album));
    update(OrderRepeatAll, repeatIs(RepeatMode::All));

    const auto shuffleIs = [this](ShuffleMode mode) {
        return [this, mode](wxUpdateUIEvent& event) {
            event.Check(playlist_.shuffle() == mode);
        };
    };
    update(OrderShuffleOff, shuffleIs(ShuffleMode::Off));
    update(OrderShuffleAlbums, shuffleIs(ShuffleMode::Albums));
    update(OrderShuffleAll, shuffleIs(ShuffleMode::All));
}

// --- opening ------------------------------------------------------------

void MainFrame::openUrls(const std::vector<Url>& urls) { addUrls(urls, -1); }

void MainFrame::openFiles() {
    std::string patterns;
    for (const std::string& extension : registry_.allExtensions()) {
        if (!patterns.empty()) {
            patterns += ';';
        }
        patterns += "*." + extension;
    }
    const wxString wildcard =
        "Audio Files|" + toWx(patterns) + "|All Files|*.*";

    wxFileDialog dialog(this, "Open Files", wxEmptyString, wxEmptyString, wildcard,
                        wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    wxArrayString chosen;
    dialog.GetPaths(chosen);

    std::vector<Url> urls;
    urls.reserve(chosen.GetCount());
    for (const wxString& path : chosen) {
        urls.push_back(Url::fromLocalPath(std::filesystem::path{path.ToStdWstring()}));
    }
    addUrls(urls);
}

void MainFrame::openFolder() {
    const wxString chosen =
        wxDirSelector("Open Folder", wxEmptyString, wxDD_DEFAULT_STYLE,
                      wxDefaultPosition, this);
    if (chosen.IsEmpty()) {
        return;
    }
    addUrls({Url::fromLocalPath(std::filesystem::path{chosen.ToStdWstring()})});
}

void MainFrame::savePlaylistAs() {
    wxFileDialog dialog(this, "Save Playlist", wxEmptyString, "playlist.m3u8",
                        "M3U Playlist (*.m3u8)|*.m3u8|PLS Playlist (*.pls)|*.pls|"
                        "XSPF Playlist (*.xspf)|*.xspf",
                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    const std::filesystem::path path{dialog.GetPath().ToStdWstring()};
    const std::string           extension = pathToUtf8Generic(path.extension());

    PlaylistFormat format = PlaylistFormat::M3u;
    if (extension == ".pls") {
        format = PlaylistFormat::Pls;
    } else if (extension == ".xspf") {
        format = PlaylistFormat::Xspf;
    }

    // PlaylistFile writes text and the caller does the file I/O, which is what
    // keeps it testable without a filesystem. The destination goes in because
    // relative paths are written against it -- which is what makes a playlist
    // survive moving a music folder wholesale.
    const std::string text = writePlaylist(format, playlist_.entries(),
                                           playlist_.queue(),
                                           Url::fromLocalPath(path));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        wxMessageBox("Could not write the playlist.", "XPCog", wxOK | wxICON_WARNING,
                     this);
        return;
    }
    setStatusText("Playlist saved.");
}

void MainFrame::addUrls(const std::vector<Url>& urls, int atRow) {
    if (urls.empty()) {
        return;
    }
    pendingScans_.push_back(ScanRequest{urls, atRow});
    pumpScanQueue();
}

void MainFrame::pumpScanQueue() {
    if (scan_ || pendingScans_.empty()) {
        return;
    }

    ScanRequest request = std::move(pendingScans_.front());
    pendingScans_.erase(pendingScans_.begin());

    scan_ = std::make_unique<ScanTask>(registry_, &cache_, std::move(request.inputs),
                                       dispatch_);

    subscriptions_.push_back(scan_->progress.connect([this](int done, int total) {
        // A range of zero is a busy indicator, which is the truthful display
        // while the expansion pass is still counting.
        if (total > 0) {
            scanBar_->SetRange(total);
            scanBar_->SetValue(done);
            // And the same number on the taskbar button, which is what Cog puts
            // on its Dock tile -- its progress bar tracks PlaylistLoader, not the
            // seek position. Left alone while the total is still zero: a bar
            // sitting at zero reads as stalled, where no bar reads as "not
            // started", which is the truth.
            taskbar_->setProgress(static_cast<double>(done) / total);
        } else {
            scanBar_->Pulse();
        }
    }));

    const int atRow = request.atRow;
    subscriptions_.push_back(scan_->finished.connect(
        [this, atRow](const std::vector<PlaylistEntry>& entries, bool cancelled) {
            // The task owns the thread it is still returning from, so it cannot
            // be destroyed from inside its own callback. Handing it to the event
            // loop to drop is what deleteLater() was doing.
            auto* finished = scan_.release();
            dispatch_([finished] { delete finished; });

            scanBar_->Hide();
            scanCancel_->Hide();
            taskbar_->clearProgress();

            addScannedEntries(entries, atRow, cancelled);
            pumpScanQueue();
        }));

    scanBar_->SetRange(0);
    scanBar_->Show();
    scanCancel_->Show();
    scan_->start();
}

void MainFrame::addScannedEntries(std::vector<PlaylistEntry> entries, int atRow,
                                  bool cancelled) {
    if (entries.empty()) {
        setStatusText(cancelled ? "Nothing was added." : "Nothing playable was found.");
        return;
    }

    // The row a drop targeted may no longer exist: the scan took time and the
    // user could have edited the playlist meanwhile. insert() clamps, so this
    // lands at the end rather than nowhere.
    const std::size_t where =
        (atRow >= 0) ? static_cast<std::size_t>(atRow) : playlist_.size();
    const std::size_t count = entries.size();

    undo_.push(std::make_unique<InsertTracksCommand>(
        playlist_, where, std::move(entries),
        "Add " + std::to_string(count) + (count == 1 ? " Track" : " Tracks")));

    setStatusText(statusSummary());
}

// --- playback -----------------------------------------------------------

void MainFrame::activateRow(unsigned int row) {
    const TrackId id = view_.trackAt(row);
    if (id != kInvalidTrackId) {
        playback_->playTrack(id);
    }
}

std::vector<TrackId> MainFrame::selectedTracks() const {
    wxDataViewItemArray items;
    list_->GetSelections(items);

    std::vector<TrackId> ids;
    ids.reserve(items.GetCount());
    for (const wxDataViewItem& item : items) {
        const TrackId id = view_.trackAt(model_->GetRow(item));
        if (id != kInvalidTrackId) {
            ids.push_back(id);
        }
    }
    return ids;
}

void MainFrame::removeSelected() {
    std::vector<TrackId> ids = selectedTracks();
    if (ids.empty()) {
        return;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        playlist_, std::move(ids),
        "Remove " + std::to_string(count) + (count == 1 ? " Track" : " Tracks")));
    setStatusText(statusSummary());
}

void MainFrame::enqueueSelected() {
    for (const TrackId id : selectedTracks()) {
        playlist_.enqueue(id);
    }
}

void MainFrame::onPositionChanged(double seconds, double duration) {
    duration_ = duration;
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);

    if (!seekBar_->scrubbing()) {
        clock_->SetLabelText(toWx(formatClock(seconds) + " / " + formatClock(duration)));
    }
    if (mini_ != nullptr && mini_->IsShown()) {
        mini_->setPosition(seconds, duration);
    }

    // The OS extrapolates from the rate it was given, so pushing every tick would
    // be four rewrites a second for a display that is already counting correctly
    // on its own. A second's drift is the threshold worth correcting.
    if (std::abs(seconds - mediaPosition_) >= 1.0) {
        mediaPosition_ = seconds;
        media_->setPlaybackState(playback_->playing(), playback_->paused(), seconds);
    }
}

void MainFrame::onCurrentTrackChanged(TrackId id) {
    currentTrack_ = id;
    view_.setCurrentTrack(id);

    const PlaylistEntry* entry = playlist_.find(id);
    const std::string    text  = entry != nullptr ? entry->display() : std::string{};

    // Both arms have to be the same type, so the literal is wrapped rather
    // than left for the conditional operator to guess at.
    SetTitle(text.empty() ? wxString("XPCog") : toWx(text + " — XPCog"));
    SetStatusText(toWx(text), 1);

    const std::string title  = entry != nullptr ? entry->title() : std::string{};
    const std::string artist = entry != nullptr ? entry->artist : std::string{};
    presence_->setNowPlaying(title, artist);
    if (mini_ != nullptr) {
        mini_->setNowPlaying(title, artist);
    }

    publishNowPlaying(id);
    refreshInfo();
}

void MainFrame::onPlaybackStateChanged(bool playing, bool paused) {
    refreshTransportIcons();
    taskbar_->setPlaybackState(playing, paused);
    presence_->setPlaybackState(playing, paused);
    if (mini_ != nullptr) {
        mini_->setPlaybackState(playing, paused);
    }

    if (!playing) {
        seekBar_->setDuration(0.0);
        clock_->SetLabelText("0:00 / 0:00");
        media_->clear();
        mediaPosition_ = -1.0;
        return;
    }
    media_->setPlaybackState(playing, paused, playback_->position());
    mediaPosition_ = playback_->position();
}

void MainFrame::publishNowPlaying(TrackId id) {
    mediaPosition_ = -1.0;

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

// --- state --------------------------------------------------------------

std::string MainFrame::statusSummary() const {
    double total = 0.0;
    for (const PlaylistEntry& entry : playlist_.entries()) {
        total += entry.duration();
    }
    const std::size_t count = playlist_.size();
    return std::to_string(count) + (count == 1 ? " track" : " tracks") + " — " +
           formatClock(total);
}

void MainFrame::setStatusText(const std::string& text) {
    SetStatusText(toWx(text), 0);
}

void MainFrame::rememberGeometry() {
    if (!IsMaximized() && !IsIconized() && IsShown()) {
        normalRect_ = GetRect();
    }
}

void MainFrame::restoreState() {
    // Window geometry lives beside the settings rather than in settings.def: it
    // is this application's own state, not a Cog setting to stay compatible with.
    if (const std::string geometry = settings_.rawValue("xpcog.window.geometry");
        !geometry.empty()) {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int maximised = 0;
        if (std::sscanf(geometry.c_str(), "%d,%d,%d,%d,%d", &x, &y, &width, &height,
                        &maximised) == 5 &&
            width > 0 && height > 0) {
            const wxRect saved(x, y, width, height);
            // Only if some display still contains it. A rectangle saved on a
            // second monitor that is no longer attached would put the window
            // somewhere the user cannot reach, and "my player will not open" is a
            // much worse bug than "my player forgot where it was".
            if (wxDisplay::GetFromPoint(saved.GetTopLeft()) != wxNOT_FOUND) {
                SetSize(saved);
                normalRect_ = saved;
            }
            if (maximised != 0) {
                Maximize(true);
            }
        }
    }

    if (const std::string root = settings_.rawValue("xpcog.fileTree.root");
        !root.empty()) {
        tree_->setRootPath(root);
    }
    if (const std::string sash = settings_.rawValue("xpcog.window.sash");
        !sash.empty()) {
        try {
            splitter_->SetSashPosition(std::stoi(sash));
        } catch (const std::exception&) {
            // A value someone edited by hand. The default is fine.
        }
    }

    // The docking layout: where each pane sits, how big it is, whether it is
    // floating and whether it is open at all. This is what QMainWindow's
    // saveState()/restoreState() carried.
    //
    // After every pane has been added, because LoadPerspective matches on the
    // names given there and silently ignores a name it does not recognise -- so
    // loading first would restore nothing and look like the setting was empty.
    if (const std::string layout = settings_.rawValue("xpcog.window.layout");
        !layout.empty()) {
        auiManager_.LoadPerspective(toWx(layout), true);
    }

    // Whatever the perspective said, the transport is on screen. A saved layout
    // from a build where it was closable would otherwise leave a window with no
    // play button and no way to get one back.
    if (wxAuiPaneInfo& transport = auiManager_.GetPane("transport"); transport.IsOk()) {
        transport.Show();
        auiManager_.Update();
    }
}

void MainFrame::persistState() {
    settings_.setRawValue("xpcog.fileTree.root", tree_->rootPath());

    if (!normalRect_.IsEmpty()) {
        settings_.setRawValue(
            "xpcog.window.geometry",
            std::to_string(normalRect_.GetX()) + "," +
                std::to_string(normalRect_.GetY()) + "," +
                std::to_string(normalRect_.GetWidth()) + "," +
                std::to_string(normalRect_.GetHeight()) + "," +
                (IsMaximized() ? "1" : "0"));
    }
    if (splitter_->IsSplit()) {
        settings_.setRawValue("xpcog.window.sash",
                              std::to_string(splitter_->GetSashPosition()));
    }

    // Only while the window is actually on screen.
    //
    // This is the trap the Qt build documented at length and it applies here for
    // the same reason: close-to-tray makes "save a layout with nothing visible"
    // the normal path, and a layout captured then is not the one the listener
    // arranged. Skipping is right rather than merely safe -- the values already
    // stored were written while the window *was* visible, so they are the last
    // true ones.
    if (IsShown()) {
        settings_.setRawValue("xpcog.window.layout",
                              toUtf8(auiManager_.SavePerspective()));
    }

    if (library_ && !library_->savePlaylist(playlist_)) {
        // Worth saying, but not worth refusing to close over.
        setStatusText("Could not save the playlist: " + library_->lastError());
    }
    settings_.sync();
}

}  // namespace xpcog::app
