#include "MainFrame.hpp"

#include "AboutDialog.hpp"
#include "AppIcon.hpp"
#include "Commands.hpp"
#include "EqualizerPanel.hpp"
#include "FileTree.hpp"
#include "InfoPanel.hpp"
#include "LyricsPanel.hpp"
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
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"
#include "xpcog/core/library/PlaylistFile.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <wx/bmpbuttn.h>
#include <wx/dataview.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/mstream.h>
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
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>
#include <vector>

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

/// Cog's consent alert, plus a route to what is being consented to.
///
/// The text is Cog's, from its own Localizable.xcstrings -- "Would you like to
/// allow Sentry to submit crash reports? You may turn this off again in
/// Preferences. We won't ask you again." -- with one sentence added naming the
/// privacy policy, because Cog's alert has nowhere to put a link and this does.
///
/// Not a wxMessageDialog for exactly that reason: a message box holds text and
/// buttons and nothing else, and a consent prompt that cannot show you the
/// policy is asking you to agree to something you have no way to read. Twenty
/// lines of sizer buys a real wxHyperlinkCtrl.
///
/// Returns true only for a deliberate yes. Closing the window is a no, which is
/// the right default for the direction this decision runs in.
[[nodiscard]] bool askConsent(wxWindow* parent) {
    wxDialog dialog(parent, wxID_ANY, "Crash reporting");

    auto* text = new wxStaticText(
        &dialog, wxID_ANY,
        "Would you like to allow Sentry to submit crash reports?\n\n"
        "You may turn this off again in Preferences. We won't ask you again.");
    text->Wrap(dialog.FromDIP(400));

    auto* policy = new wxHyperlinkCtrl(
        &dialog, wxID_ANY, "Privacy policy",
        wxString::FromUTF8(std::string{platform::kPrivacyPolicyUrl}));

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(text, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    layout->Add(policy, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    if (wxSizer* buttons = dialog.CreateStdDialogButtonSizer(wxYES | wxNO);
        buttons != nullptr) {
        layout->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
                    dialog.FromDIP(12));
    }
    dialog.SetSizerAndFit(layout);
    dialog.CenterOnParent();

    // No is the one that needs saying. wxDialogBase::OnButton ends the dialog for
    // the *affirmative* id, which CreateStdDialogButtonSizer sets to wxID_YES for
    // this flag pair, and for the escape id, which it sets to nothing at all --
    // so Yes works by itself and No takes the click and sits there
    // (wxWidgets/src/common/dlgcmn.cpp). Yes is bound too rather than left to the
    // default, so that both answers leave by the same route and neither depends
    // on which button the sizer happened to consider affirmative.
    dialog.Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_YES); },
                wxID_YES);
    dialog.Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_NO); },
                wxID_NO);

    return dialog.ShowModal() == wxID_YES;
}

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
        // And reported, when there is consent to report it. This is the shape
        // Cog's captureMessage calls have -- a thing that should have worked and
        // did not, on a path that then carries on regardless, which is exactly
        // the kind nobody files a bug about because nothing appears to be wrong.
        platform::reportProblem("Library would not open: " + library_->lastError());
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
        restorePlayback();
    }
    // Restoring the saved playlist is not an edit the user made, so it must not
    // be the first thing Undo offers to take back.
    undo_.clear();

    // The mini player, if that is where the listener left off. Cog restores it at
    // launch from the same key (AppController.m:314).
    //
    // Queued rather than done here, and for a harder reason than the consent
    // prompt below: setMiniMode(true) hides this frame, and XPCogApp::OnInit
    // calls Show() on it *after* this constructor returns. Restoring in place
    // would be undone one line later by the code that opens the window, which is
    // the kind of ordering bug that looks like the setting not being saved.
    //
    // Before the consent prompt, so that the prompt's parent is whichever window
    // is actually on screen.
    if (settings_.MiniMode()) {
        CallAfter([this] { setMiniMode(true); });
    }

    // Cog asks as the window appears (Window/MainWindow.m:57). Queued rather
    // than called here for the one difference between the two: this constructor
    // runs before XPCogApp shows the frame, and a modal dialog whose parent is
    // not on screen yet is a dialog floating over nothing. CallAfter lands on
    // the first turn of the event loop, by which time the window is up.
    CallAfter([this] { askCrashReportingConsent(); });
}

MainFrame::~MainFrame() {
    // The scan borrows the registry and the PluginCache, and the cache is a
    // member of this window, so the task has to go first -- otherwise its thread
    // outlives what it is reading from. ~ScanTask cancels and joins, so nothing
    // is still posting to the interface after this returns.
    scan_.reset();

    // The tray icon is not a child window and so is not covered by the sweep
    // below. Removing it here rather than only on the quit path means it cannot
    // outlive the window it raises.
    if (presence_) {
        presence_->RemoveIcon();
    }

    // Not optional, and not something destructor ordering can substitute for:
    // the manager holds pointers to windows that are about to be destroyed, and
    // UnInit() is what detaches it from them first.
    auiManager_.UnInit();

    // Then every widget, explicitly, while the things they borrow are still
    // alive.
    //
    // This is the ordering trap of the whole class, and it is not visible from
    // any one line of it. A frame's children are destroyed by ~wxWindow, which
    // runs *after* the frame's own members -- so by default the spectrum panel
    // outlives the AudioTap it holds a reference to, the data model outlives the
    // PlaylistView it reads, and the SC-55 panel outlives the controller its
    // position callback calls into. Every one of those is a read of a destroyed
    // object during teardown.
    //
    // DestroyChildren() moves the whole sweep to a point where playback_, view_
    // and library_ are all still valid. The pointers left behind are cleared
    // because nothing should be tempted to follow them afterwards.
    DestroyChildren();

    dockHost_  = nullptr;
    splitter_  = nullptr;
    tree_      = nullptr;
    list_      = nullptr;
    model_     = nullptr;
    seekBar_   = nullptr;
    volume_    = nullptr;
    filter_    = nullptr;
    clock_     = nullptr;
    scanBar_   = nullptr;
    scanCancel_ = nullptr;
    equalizer_ = nullptr;
    info_      = nullptr;
    lyrics_    = nullptr;
    spectrum_  = nullptr;
    mini_      = nullptr;
#ifdef XPCOG_HAVE_SC55_PANEL
    sc55_ = nullptr;
#endif
    transportButtons_.clear();
    playPauseButton_ = nullptr;
}

void MainFrame::buildUi() {
    // The transport strip is not a wxAUI pane, and that is the fix rather than an
    // oversight.
    //
    // wxAUI gives a dock exactly two layout modes and neither is what a transport
    // strip wants. A dock is *fixed* when every pane in it is fixed, or when any
    // pane sets DockFixed -- LayoutAll() decides that -- and LayoutAddDock() then
    // lays a fixed dock's panes out at pane.best_size and adds a stretchable
    // background spacer after them to swallow whatever width is left. That spacer
    // is the empty half-window this strip has been sitting beside: not a missing
    // proportion, a deliberate one. Make the dock non-fixed instead and the pane
    // does fill the width, but LayoutAddDock() then puts a drag sash under a top
    // dock, so the height of a row of fixed-height controls becomes something the
    // user can pull around.
    //
    // Full width and a fixed height cannot both be asked for of a docked pane, so
    // the strip stops being one. It was a pane in name only in any case: dockable,
    // floatable, movable, closable and captioned were all already switched off,
    // which is every single thing wxAUI would have been managing it for. And
    // wxAuiManager manages any window rather than only a frame, so it takes the
    // panel below the strip and the frame's own sizer stacks the two.
    //
    // A saved perspective from before this still names a "transport" pane;
    // LoadPerspective() skips names it cannot find, so it costs nothing.
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* transport = new wxPanel(this, wxID_ANY);
    buildTransport(transport);
    root->Add(transport, 0, wxEXPAND);

    dockHost_ = new wxPanel(this, wxID_ANY);
    root->Add(dockHost_, 1, wxEXPAND);
    SetSizer(root);

    auiManager_.SetManagedWindow(dockHost_);

    // The playlist and the file browser are the centre, and the browser is a
    // splitter pane rather than a dock: Cog's file tree is a fixed part of the
    // window and behaves as one, and the View menu toggles the split.
    splitter_ = new wxSplitterWindow(dockHost_, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize,
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

    // The optional panes, in the places the Qt build docked them: the wide, short
    // ones along the bottom and the tall column of fields at the right.
    equalizer_ = new EqualizerPanel(dockHost_, settings_);
    info_      = new InfoPanel(dockHost_, library_.get());
    lyrics_    = new LyricsPanel(dockHost_);
    spectrum_  = new SpectrumPanel(dockHost_, playback_->tap());
    spectrum_->applySettings(settings_);

#ifdef XPCOG_HAVE_SC55_PANEL
    sc55_ = new Sc55Panel(dockHost_, [this] { return playback_->position(); });
#endif

    // Every pane is named, and the names are what a saved perspective refers to.
    // Renaming one silently discards that pane's saved position, so these are as
    // load-bearing as the object names QMainWindow::saveState() needed.
    auiManager_.AddPane(splitter_, wxAuiPaneInfo().Name("playlist").CenterPane());

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

    // Beside Info rather than under the playlist, which is where Cog puts its
    // lyrics window too -- both are "about the track you are looking at", and on
    // the right they tab together instead of competing for the same edge.
    //
    // Taller than it is wide, and the minimum says so: a verse wrapped into a
    // 200-pixel column is unreadable in a way a truncated tag field is not.
    auiManager_.AddPane(lyrics_, wxAuiPaneInfo()
                                     .Name("lyrics")
                                     .Caption("Lyrics")
                                     .Right()
                                     .BestSize(FromDIP(wxSize(320, 480)))
                                     .MinSize(FromDIP(wxSize(240, 160)))
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
        // Widened explicitly. MPRIS carries a float and setVolume() takes a
        // double, so the conversion happens either way; saying so is what keeps
        // -Wdouble-promotion quiet, and the tree warning-free is a property that
        // is only worth anything while it is actually true.
        playback_->setVolume(static_cast<double>(gain));
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
    // playing track otherwise. The lyrics pane follows the same rule, from the
    // same event -- Cog observes the selection separately in each of its two
    // controllers, which is the same wiring with the duplication in a different
    // place.
    list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) {
        refreshInfo();
        refreshLyrics();
    });

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
        if (event.GetPane() == nullptr) {
            return;
        }
        // Neither clock is the manager's to stop, and neither panel watches for
        // being hidden any more.
        if (event.GetPane()->window == spectrum_) {
            spectrum_->setActive(false);
        }
#ifdef XPCOG_HAVE_SC55_PANEL
        if (event.GetPane()->window == sc55_) {
            sc55_->setActive(false);
        }
#endif
    });

    bindCommands();
    bindUpdateUi();
}

void MainFrame::onSettingChanged(const std::string& key) {
    // The equaliser and the DSP chain are read by the engine when it is asked to,
    // so a band that moves has to say so or the slider does nothing until the
    // next track.
    if (key.starts_with("eq") || key == "GraphicEQenable" ||
        key == "enableFSurround" || key == "enableFading" ||
        key == "volumeScaling" || key == "enableHDCD") {
        playback_->reloadDsp();
        return;
    }

    // Turning genre tracking on applies the playing track's genre at once. Cog's
    // -toggleTracking: does the same, and the reason is that the alternative --
    // waiting for the next track -- makes the checkbox look like it did nothing.
    // Turning it *off* deliberately leaves the curve where it is: the last
    // preset it chose is as good a starting point as any, and silently flipping
    // back to some remembered curve would be a second surprise.
    if (key == "GraphicEQtrackgenre") {
        if (settings_.GraphicEqTrackGenre()) {
            // Past the memo deliberately: the playing track has almost certainly
            // been matched already, and the whole point of the toggle is to act
            // on it now.
            lastGenreTrack_ = kInvalidTrackId;
            lastGenre_.clear();
            applyGenreEqualizer(playlist_.find(currentTrack_));
        }
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
        return;
    }

    // The View menu is where this is normally changed, and that path refreshes
    // the panes itself. This is the other one: the generated row in Advanced,
    // which every setting gets. Without it, changing the mode there does nothing
    // visible until the next selection or track change, which reads as the row
    // being inert.
    if (key == "panelFollowMode") {
        refreshInfo();
        refreshLyrics();
        return;
    }

    // The playlist's own three, which it holds as state rather than reading when
    // it needs them. The constructor seeds all three and, until this branch,
    // nothing ever seeded them again -- so "Stop after every track" in
    // Preferences did nothing at all until the next launch, and silently:
    // the box stays ticked, the setting is stored, and playback simply carries
    // on to the next track.
    //
    // Repeat and Shuffle escaped notice because the Order menu sets them on the
    // playlist directly as well as storing them. Their rows in Advanced had
    // exactly the same defect.
    if (key == "alwaysStopAfterCurrent") {
        playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());
        return;
    }
    if (key == "repeat") {
        playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
        return;
    }
    if (key == "shuffle") {
        playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
        return;
    }

    // The same shape once more, with a slider instead of a menu in front of it.
    // Volume is seeded into the engine by PlaybackController's constructor and
    // into the slider by buildUi(), and both keep it from then on -- so the row
    // in Advanced moved a number nothing read again. The slider has to be moved
    // too, or the interface disagrees with what is coming out of the speakers.
    if (key == "volume") {
        const double gain = settings_.Volume();
        volume_->SetValue(static_cast<int>(std::lround(gain * 100.0)));
        playback_->setVolume(gain);
        media_->setVolume(static_cast<float>(gain));
        return;
    }

    // Immediately, in both directions, which is the half of Cog's arrangement
    // that is easy to leave out: its observer on `sentryConsented` calls
    // `[SentrySDK close]` the moment the box is unticked
    // (AppController.m:417-420), rather than waiting for a relaunch. Anything
    // else means unticking the box and still being reported on for the rest of
    // the session.
    if (key == "sentryConsented") {
        if (settings_.SentryConsented()) {
            platform::startCrashReporting();
        } else {
            platform::stopCrashReporting();
        }
        settings_.sync();
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

TrackId MainFrame::panelTrackId() const {
    // Following playback ignores the selection entirely, which is the whole
    // point: the panes stay on what is playing while the playlist is browsed.
    // No fallback in this direction -- with nothing playing the panes are empty,
    // and that is the honest answer rather than quietly reverting to the other
    // mode the moment it would have something to show.
    if (settings_.PanelFollowMode() == 1) {
        return currentTrack_;
    }

    // Cog's rule, from both of its controllers (InfoWindowController and
    // LyricsWindowController.m:33-43, which observe the selection and the
    // current entry and prefer the selection exactly like this).
    const std::vector<TrackId> selection = selectedTracks();
    return selection.empty() ? currentTrack_ : selection.front();
}

void MainFrame::refreshInfo() {
    // Returns immediately while the panel is hidden, which is most of the time --
    // and matters, because metadata arriving during a scan would otherwise redraw
    // twenty fields per file.
    if (!paneShown(info_)) {
        return;
    }
    info_->showEntry(playlist_.find(panelTrackId()));
}

void MainFrame::refreshLyrics() {
    if (!paneShown(lyrics_)) {
        return;
    }
    lyrics_->showEntry(playlist_.find(panelTrackId()));
}

void MainFrame::restorePlayback() {
    // Cog's shape (AppController.m:266-292): if the last session was not stopped,
    // find the entry the library marked current and *select* it -- always -- and
    // start it only if the listener asked for that. Selecting either way is the
    // part worth copying: coming back to a playlist with the last thing you were
    // listening to highlighted is useful even to someone who does not want it
    // playing the moment the window opens.
    const int last = settings_.LastPlaybackStatus();
    if (last == 0) {
        return;
    }

    const auto current = playlist_.current();
    if (!current) {
        return;
    }

    if (const auto row = view_.rowForTrack(*current)) {
        const wxDataViewItem item = model_->GetItem(static_cast<unsigned>(*row));
        list_->Select(item);
        list_->EnsureVisible(item);
    }

    if (!settings_.ResumePlaybackOnStartup()) {
        return;
    }

    double position = 0.0;
    try {
        const std::string stored = settings_.rawValue("xpcog.playback.position");
        position = stored.empty() ? 0.0 : std::stod(stored);
    } catch (const std::exception&) {
        // A value that will not parse is a position we do not have, not a reason
        // to refuse to play. The top of the track is the honest fallback.
        position = 0.0;
    }

    // Queued, for the reason the mini player's restore is: this runs from the
    // constructor, and starting playback before the window exists means the first
    // track change redraws widgets that are still being built.
    const TrackId id = *current;
    CallAfter([this, id, position, last] {
        playback_->resumeTrack(id, position, last == 2);
    });
}

void MainFrame::applyGenreEqualizer(const PlaylistEntry* entry) {
    if (!settings_.GraphicEqTrackGenre()) {
        return;
    }

    if (entry == nullptr) {
        // Stopped, or the track failed. Forgetting what was matched is what lets
        // the same track be matched again when it is played again.
        lastGenreTrack_ = kInvalidTrackId;
        lastGenre_.clear();
        return;
    }

    // Once per track and genre, not once per call -- see the members for why
    // this handler runs more than once for one track, and what an unguarded
    // second run would cost.
    if (entry->id == lastGenreTrack_ && entry->genre == lastGenre_) {
        return;
    }
    lastGenreTrack_ = entry->id;
    lastGenre_      = entry->genre;

    const EqualizerPresetLibrary& library = shippedEqualizerPresets();
    const int                     index   = library.matchGenre(entry->genre);
    const EqualizerPreset*        preset  = library.at(index);
    if (preset == nullptr) {
        // No library shipped, so there is no preset to choose. Leaving the curve
        // alone is the only sensible answer: the setting asked for a genre's
        // preset, not for the equaliser to be reset.
        return;
    }

    settings_.setGraphicEqPreset(index);
    applyEqualizerPreset(settings_, *preset);
    if (equalizer_ != nullptr) {
        equalizer_->refresh();
    }
    playback_->reloadDsp();
}

void MainFrame::notifyTrack(const PlaylistEntry* entry) {
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
    // onCurrentTrackChanged is a redraw-everything handler and is *meant* to run
    // more than once for one track: PlaybackController publishes when the decoder
    // opens the track (PlaybackController.cpp:213) and again when the gapless
    // seam reaches the speaker (:392), and trackMetadataChanged calls it a third
    // time whenever a stream renames itself. Redrawing a title bar twice costs
    // nothing. Announcing a track twice is two notifications.
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
        subtitle = entry->artist + " - " + entry->album;
    } else if (!entry->artist.empty()) {
        subtitle = entry->artist;
    } else {
        subtitle = entry->album;
    }

    std::string body = entry->title();
    if (!subtitle.empty()) {
        body += "\n" + subtitle;
    }

    // The cover, decoded from the library the same way the info panel decodes it.
    // Cog writes the art to a temp file because UNNotificationAttachment takes a
    // URL (PlaybackEventController.m:190-200); wx takes a wxIcon, so nothing
    // touches the disk here.
    wxIcon cover;
    if (settings_.NotificationsShowAlbumArt() && library_ && !entry->artHash.empty()) {
        const std::vector<std::byte> bytes = library_->artwork(entry->artHash);
        if (!bytes.empty()) {
            wxMemoryInputStream stream(bytes.data(), bytes.size());
            wxImage             image;
            if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.IsOk()) {
                // Scaled down first. A balloon draws this at icon size, and
                // handing it a 1500-pixel scan means the platform rescales a
                // megabyte of cover art on the interface thread once a track.
                const int side = FromDIP(48);
                image.Rescale(side, side, wxIMAGE_QUALITY_HIGH);
                cover.CopyFromBitmap(wxBitmap(image));
            }
        }
    }

    presence_->notify("Now Playing", body, cover);
}

void MainFrame::setMiniMode(bool mini) {
    // Recorded as it changes, which is where Cog records it
    // (AppController.m:1027, in -setMiniMode: itself) rather than on the way out.
    // The difference matters after a crash: the mode you were last in is the one
    // you come back to, instead of the one you were in the last time the
    // application managed to exit tidily.
    settings_.setMiniMode(mini);

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

void MainFrame::askCrashReportingConsent() {
    if (!platform::crashReportingAvailable() || settings_.SentryAskedConsent()) {
        return;
    }

    // Recorded *before* the answer, which is what Cog does and is not an
    // oversight in either place (Window/MainWindow.m:36 writes the flag outside
    // the completion handler). The promise is "we won't ask you again", and it
    // has to hold for the person who closed the dialog without answering just as
    // much as for the one who pressed No -- otherwise declining to decide is the
    // one response that gets asked again every launch.
    settings_.setSentryAskedConsent(true);

    // Whichever window is actually on screen. Cog has two prompts for this, one
    // per window class; here there is one, and it asks which mode it is in.
    wxWindow* parent = (mini_ != nullptr && mini_->IsShown())
                           ? static_cast<wxWindow*>(mini_)
                           : static_cast<wxWindow*>(this);

    if (!askConsent(parent)) {
        // No is already the stored default; writing it anyway so that the answer
        // is a value someone can see rather than an absence they have to infer.
        settings_.setSentryConsented(false);
        settings_.sync();
        return;
    }

    settings_.setSentryConsented(true);
    // Flushed here rather than at quit: this is the one setting whose whole
    // point is to be read on the *next* launch, including the launch after a
    // crash, and a crash is precisely the exit that never reaches Settings::sync.
    settings_.sync();
    platform::startCrashReporting();
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
    on(ViewLyrics, [this] {
        const bool showing = !paneShown(lyrics_);
        togglePane(lyrics_, showing);
        // Drawn on the way in, because refreshLyrics() declines while hidden --
        // so a pane opened between track changes would otherwise stay blank
        // until the next one.
        if (showing) {
            refreshLyrics();
        }
    });

    // Both panes redraw, because the mode is what decides which track they show
    // and neither would otherwise notice until the next selection or track
    // change -- which, for someone who switched to Follow Playback precisely so
    // that clicking around stops moving the panes, could be the rest of the song.
    const auto follow = [this](int mode) {
        settings_.setPanelFollowMode(mode);
        refreshInfo();
        refreshLyrics();
    };
    on(ViewFollowSelection, [follow] { follow(0); });
    on(ViewFollowPlayback, [follow] { follow(1); });
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
    on(ViewSc55Panel, [this] {
        const bool showing = !paneShown(sc55_);
        togglePane(sc55_, showing);
        // Driven from here rather than from a wxEVT_SHOW handler on the panel;
        // see SpectrumPanel.cpp for what that cost.
        sc55_->setActive(showing);
    });
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
    update(ViewLyrics,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(lyrics_)); });
    // Read from the setting rather than from a remembered flag, so the tick is
    // right on the first idle after launch without anything having to restore it.
    update(ViewFollowSelection, [this](wxUpdateUIEvent& event) {
        event.Check(settings_.PanelFollowMode() != 1);
    });
    update(ViewFollowPlayback, [this](wxUpdateUIEvent& event) {
        event.Check(settings_.PanelFollowMode() == 1);
    });
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

    // The queue is translated from ids to positions, and that is a real
    // conversion rather than a cast to satisfy a signature. Playlist::queue()
    // holds TrackIds, which are opaque and permanent; writePlaylist wants
    // indices into the entries it is being handed, because that is what Cog's
    // XML stores and what readPlaylist gives back. Writing ids where positions
    // belong produced a saved playlist whose queue pointed at whatever rows
    // those numbers happened to name.
    //
    // It compiled on Windows for two years for a silly reason: TrackId is
    // uint64_t, and MSVC's size_t is unsigned long long, so the two vectors were
    // the same type. On macOS and Linux size_t is unsigned long -- same width,
    // different type -- and the first compiler that was not MSVC rejected it
    // immediately. An overload that took either would have hidden this for good.
    std::vector<std::size_t> queuePositions;
    queuePositions.reserve(playlist_.queue().size());
    for (const TrackId id : playlist_.queue()) {
        if (const std::optional<std::size_t> index = playlist_.indexOf(id); index) {
            queuePositions.push_back(*index);
        }
    }

    // PlaylistFile writes text and the caller does the file I/O, which is what
    // keeps it testable without a filesystem. The destination goes in because
    // relative paths are written against it -- which is what makes a playlist
    // survive moving a music folder wholesale.
    const std::string text = writePlaylist(format, playlist_.entries(), queuePositions,
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

    // Read per scan rather than once, so unticking the box in Preferences applies
    // to the next folder added instead of the next launch.
    //
    // Cog skips .cue files while walking a folder when this is off
    // (PlaylistLoader.m:264-282), which is what stops a folder holding album.cue
    // beside album.flac from adding every track twice -- once through the cue
    // sheet and once as the whole file. The Scanner has always honoured the
    // option; nothing ever set it from the setting.
    Scanner::Options scanOptions;
    scanOptions.readCueSheets = settings_.ReadCueSheetsInFolders();
    scanOptions.readPlaylists = settings_.ReadPlaylistsInFolders();

    scan_ = std::make_unique<ScanTask>(registry_, &cache_, std::move(request.inputs),
                                       dispatch_, scanOptions);

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

    // Cog moves the selection as each next entry is *chosen*, inside
    // -getNextEntry: (PlaylistController.m:1448-1522, six call sites). Done here
    // instead, as the track actually becomes current, which lands on the same
    // rows in the same order and needs one call site rather than six -- the
    // playlist here has no equivalent hook, because choosing the next entry is
    // core's job and selecting a row is the interface's.
    //
    // EnsureVisible as well as Select: a selection that has scrolled out of
    // sight is one the listener has to go looking for, which is the opposite of
    // what following playback is for.
    if (settings_.SelectionFollowsPlayback() && id != kInvalidTrackId) {
        if (const auto row = view_.rowForTrack(id)) {
            const wxDataViewItem item = model_->GetItem(static_cast<unsigned>(*row));
            list_->UnselectAll();
            list_->Select(item);
            list_->EnsureVisible(item);
        }
    }

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
    refreshLyrics();
    notifyTrack(entry);
    applyGenreEqualizer(entry);
}

void MainFrame::onPlaybackStateChanged(bool playing, bool paused) {
    // Recorded as it changes rather than at exit, which is Cog's vocabulary and
    // the safer moment: a player that crashed while stopped must not come back
    // playing, and a status written only on a tidy exit says nothing about the
    // session that did not have one.
    settings_.setLastPlaybackStatus(!playing ? 0 : (paused ? 2 : 1));

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

    // Nothing here forces the transport back on screen any more, and nothing needs
    // to: it is no longer a pane, so no perspective can hide it.
}

void MainFrame::persistState() {
    settings_.setRawValue("xpcog.fileTree.root", tree_->rootPath());

    // Where the current track had got to. A raw key rather than a settings.def
    // entry because it is this application's own state, like the window geometry
    // below it -- Cog keeps the equivalent as a Core Data attribute on the entry
    // (`currentPosition`), which is not a preference either.
    //
    // Which entry it belongs to needs no recording: the library already marks the
    // current one, and loadPlaylist restores it.
    settings_.setRawValue("xpcog.playback.position",
                          std::to_string(playback_->position()));

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
