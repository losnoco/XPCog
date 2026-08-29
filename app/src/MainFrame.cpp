#include "MainFrame.hpp"

#include "AboutDialog.hpp"
#include "AppIcon.hpp"
#include "Commands.hpp"
#include "EqualizerPanel.hpp"
#include "FileTree.hpp"
#include "InfoPanel.hpp"
#include "LastFmAccount.hpp"
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
#include "xpcog/platform/FileManager.hpp"
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
#include <wx/richmsgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/toolbar.h>
#include <wx/translation.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <fstream>
#include <set>
#include <unordered_map>
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
    wxDialog dialog(parent, wxID_ANY, _("Crash reporting"));

    auto* text = new wxStaticText(
        &dialog, wxID_ANY,
        _("Would you like to allow Sentry to submit crash reports?\n\n"
          "You may turn this off again in Preferences. We won't ask you again."));
    text->Wrap(dialog.FromDIP(400));

    auto* policy = new wxHyperlinkCtrl(
        &dialog, wxID_ANY, _("Privacy policy"),
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
        setStatusText(wxString::Format(_("Library unavailable: %s"),
                                       toWx(library_->lastError())));
        // And reported, when there is consent to report it. This is the shape
        // Cog's captureMessage calls have -- a thing that should have worked and
        // did not, on a path that then carries on regardless, which is exactly
        // the kind nobody files a bug about because nothing appears to be wrong.
        platform::reportProblem("Library would not open: " + library_->lastError());
        library_.reset();
    }

    playback_ =
        std::make_unique<PlaybackController>(registry_, playlist_, settings_, dispatch_);

    wireScrobbling();

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
    toolBar_ = nullptr;
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
    //
    // The strip is only the controls now. The buttons are not in that sizer at
    // all -- they are the frame's toolbar, which the frame reserves a band for
    // and places itself, above whatever the sizer lays out. So the window is
    // three bands rather than two: toolbar, controls, docks.
    buildToolBar();

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* controls = new wxPanel(this, wxID_ANY);
    buildControls(controls);
    root->Add(controls, 0, wxEXPAND);

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

    // Closed, and closed on a first launch rather than only after somebody has
    // shut it: a music player opens onto the music somebody has already added,
    // and a folder tree pointing at the home directory is a filing cabinet
    // standing where the playlist should be. Cog's is a fixed part of its window
    // and this is a deliberate difference from it -- Ctrl+B, the View menu and
    // the restored layout all bring it straight back, and whether it was open is
    // remembered from then on.
    //
    // Initialize() rather than splitting and unsplitting: an unsplit splitter has
    // one window, and it is the playlist. The tree is hidden by hand first
    // because a child that the splitter is not managing would otherwise be drawn
    // over the top of it.
    tree_->Hide();
    splitter_->Initialize(list_);

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

    // The captions here are placeholders: applyPaneCaptions() below writes the
    // translated ones, and is also what puts them back after a saved
    // perspective has restored whatever language the layout was stored in.
    auiManager_.AddPane(spectrum_, wxAuiPaneInfo()
                                       .Name("spectrum")
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
                                        .Bottom()
                                        .BestSize(equalizerBest)
                                        .MinSize(FromDIP(240), equalizerBest.GetHeight())
                                        .Hide());

    // The minimum is a floor, not a recommendation. It used to be set at the
    // width the panel reads *well* at, which conflated two different jobs: the
    // best size is what the pane opens at and is the opinion about how wide this
    // wants to be, while the minimum is only the point past which dragging
    // stops. Setting the second to the first means somebody who wants the
    // playlist wide and the panels narrow is refused for their own good.
    //
    // Both are now half what they were. Worth knowing where the real floor is:
    // the form inside Info measures 143 DIP wide -- caption column plus the 48
    // its value controls ask for -- so between 110 and there the captions clip
    // before the pane stops shrinking.
    auiManager_.AddPane(info_, wxAuiPaneInfo()
                                   .Name("info")
                                   .Right()
                                   .BestSize(FromDIP(wxSize(300, 400)))
                                   .MinSize(FromDIP(wxSize(110, 100)))
                                   .Hide());

    // Beside Info rather than under the playlist, which is where Cog puts its
    // lyrics window too -- both are "about the track you are looking at", and on
    // the right they tab together instead of competing for the same edge.
    //
    // Taller than it is wide, and the *best* size is what says so: a verse
    // wrapped into a narrow column is hard to read, which is an argument about
    // what this should open at rather than about what it may be dragged to.
    auiManager_.AddPane(lyrics_, wxAuiPaneInfo()
                                     .Name("lyrics")
                                     .Right()
                                     .BestSize(FromDIP(wxSize(320, 480)))
                                     .MinSize(FromDIP(wxSize(120, 80)))
                                     .Hide());

#ifdef XPCOG_HAVE_SC55_PANEL
    auiManager_.AddPane(sc55_, wxAuiPaneInfo()
                                   .Name("sc55")
                                   .Bottom()
                                   .BestSize(FromDIP(wxSize(420, 200)))
                                   .MinSize(FromDIP(wxSize(200, 100)))
                                   .Hide());
#endif

    applyPaneCaptions();
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
    scanCancel_->SetToolTip(_("Stop reading files"));
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

void MainFrame::buildToolBar() {
    // Everything on the strip that is a button, and nothing else.
    //
    // A wxToolBar rather than a row of wxBitmapButtons: it gets the platform's
    // own spacing, hover and pressed drawing, its tools raise wxEVT_TOOL --
    // which is wxEVT_MENU under another name -- and it answers EVT_UPDATE_UI for
    // its own tools every idle. The buttons had none of that: each needed a
    // second Bind for wxEVT_BUTTON, none could show a pressed state at all, and
    // they were spaced by a hand-picked FromDIP(2).
    //
    // The *frame's* toolbar, which is what CreateToolBar() makes it: the frame
    // reserves a band for it above the client area and positions it there, so it
    // is not in the sizer below and cannot be squeezed by what is. That is also
    // what puts it in the title bar on macOS -- wxOSX builds a native NSToolbar
    // when a toolbar's parent is a wxFrame, and wxFrame::SetToolBar installs it
    // in the window, hiding the wx window it was drawn in. So the transport
    // looks like a Mac toolbar on macOS and a toolbar row on the other two,
    // which is the point rather than a difference to paper over.
    toolBar_ = CreateToolBar(wxTB_HORIZONTAL | wxTB_FLAT | wxTB_NODIVIDER);

    for (const ToolbarItem& item : toolbarLayout()) {
        if (item.separatorBefore) {
            toolBar_->AddSeparator();
        }
        const std::string glyph = commandIcon(item.id);
        // The label is passed even though nothing draws it -- these are icon-only
        // tools -- because it is what a screen reader announces, and on macOS it
        // is also the name the native toolbar's overflow menu shows.
        toolBar_->AddTool(item.id, commandLabel(item.id), lucideIcon(glyph),
                          lucideIconDisabled(glyph), toWxItemKind(item.kind),
                          commandTooltip(item.id));
    }

    // Required, and not a formality: tools added before Realize() exist in the
    // list and are not on screen until it runs.
    toolBar_->Realize();

    // Play/Pause is settled once here as well, so it opens saying "Play" rather
    // than the table's "Play/Pause" and only correcting itself at the first
    // track. A restored session that comes back paused wants the same.
    refreshTransportIcons();
}

void MainFrame::buildControls(wxWindow* parent) {
    // The seek bar, the clock, the volume and the filter -- the things on the
    // strip that are not buttons, on a panel of their own under the toolbar.
    //
    // They could go on the toolbar with AddControl(), and should not, for two
    // separate reasons. A toolbar sizes a control to the tool height and centres
    // it, which is the wrong answer for a bar that has to stretch and a slider
    // that should not be as tall as a button. And a control on a native
    // NSToolbar is an item the toolbar lays out and may push into an overflow
    // menu, which is not somewhere a seek bar can do its job. On a panel they
    // are laid out by an ordinary sizer, which is what "stretch this one and
    // leave the rest at their best size" is spelled in.
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    seekBar_ = new SeekBar(parent, kSeekBarId);
    row->Add(seekBar_, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));

    clock_ = new wxStaticText(parent, wxID_ANY, "0:00 / 0:00", wxDefaultPosition,
                              FromDIP(wxSize(90, -1)), wxALIGN_CENTRE_HORIZONTAL);
    row->Add(clock_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    volume_ = new wxSlider(parent, kVolumeId,
                           static_cast<int>(settings_.Volume() * 100.0), 0, 100,
                           wxDefaultPosition, FromDIP(wxSize(110, -1)));
    volume_->SetToolTip(_("Volume"));
    row->Add(volume_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    filter_ = new wxSearchCtrl(parent, kFilterId, wxEmptyString, wxDefaultPosition,
                               FromDIP(wxSize(200, -1)));
    filter_->ShowCancelButton(true);
    filter_->SetDescriptiveText(_("Filter"));
    // The descriptive text disappears the moment somebody types, which is when
    // "what was this box for" starts being asked.
    filter_->SetToolTip(_("Filter the playlist"));
    row->Add(filter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    // A little air above and below, which the toolbar beside it used to be
    // providing: on its own row the strip would otherwise sit flush against the
    // toolbar's edge and the playlist's.
    auto* pad = new wxBoxSizer(wxVERTICAL);
    pad->Add(row, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
    parent->SetSizer(pad);
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
        setStatusText(entry != nullptr
                          ? wxString::Format(_("Connecting to %s..."),
                                             toWx(entry->title()))
                          : wxString(_("Connecting...")));
    });
    observe(playback_->playbackFailed,
            [this](TrackId, const std::string& reason) {
                // Already translated: PlaybackController is app-layer and
                // publishes the sentence it wants shown, not a code.
                setStatusText(toWx(reason));
            });
    // Same treatment, and for the same reason. What differs is what it is about:
    // the search for a playable track rather than any one row of the playlist.
    observe(playback_->statusNote,
            [this](const std::string& note) { setStatusText(toWx(note)); });
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

    list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent& event) {
        showPlaylistMenu(event.GetItem());
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

        // After the event, not in it. macOS sends this from
        // -outlineView:didClickTableColumn: and only *then* decides what the
        // click did to the sort descriptors -- a fresh column gets an ascending
        // one, an already-sorted column is toggled by the table view itself --
        // so an arrow set here is overwritten a moment later. CallAfter puts it
        // back once the click has finished being handled.
        CallAfter([this] { showSortIndicator(); });
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
        refreshTransportIcons(Restroke::Yes);
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
                presence_->notify(
                    toUtf8(_("XPCog is still running")),
                    toUtf8(_("Playback continues. Use the tray icon to bring the "
                             "window back or to quit.")));
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
    if (key == "enableAudioScrobbler") {
        if (scrobbler_) {
            scrobbler_->setEnabled(settings_.EnableScrobbling());
        }
        return;
    }

    // The equaliser and the DSP chain are read by the engine when it is asked to,
    // so a band that moves has to say so or the slider does nothing until the
    // next track.
    if (key.starts_with("eq") || key == "GraphicEQenable" ||
        key == "enableFSurround" || key == "enableFading" ||
        key == "volumeScaling" || key == "enableHDCD" ||
        key == "pitch" || key == "tempo" || key.starts_with("rubberband")) {
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

void MainFrame::showFileTree(bool show) {
    if (show == splitter_->IsSplit()) {
        return;
    }
    if (show) {
        splitter_->SplitVertically(tree_, list_,
                                   fileTreeSash_ > 0 ? fileTreeSash_ : FromDIP(260));
    } else {
        // Kept here rather than left to the splitter. wxSplitterWindow sets its
        // sash position to zero on Unsplit, so asking it afterwards answers with
        // the left edge -- and the browser would come back at the default width
        // every time instead of at the width it was dragged to.
        fileTreeSash_ = splitter_->GetSashPosition();
        splitter_->Unsplit(tree_);
    }
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

    // The cover, decoded from the library the same way the info panel decodes it.
    // Cog writes the art to a temp file because UNNotificationAttachment takes a
    // URL (PlaybackEventController.m:190-200); wx takes a wxIcon, so nothing
    // touches the disk here.
    wxIcon cover;
    if (settings_.NotificationsShowAlbumArt() && library_ && !entry->artHash.empty()) {
        // Shared rather than copied: the same cover is wanted by the info
        // panel and the now-playing display, and it is only being read from.
        const auto bytes = library_->sharedArtwork(entry->artHash);
        if (bytes && !bytes->empty()) {
            wxMemoryInputStream stream(bytes->data(), bytes->size());
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

    presence_->notify(toUtf8(_("Now Playing")), body, cover);
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
    PreferencesDialog dialog(this, settings_, lastFm_.get(), scrobbler_.get());
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

void MainFrame::refreshTransportIcons(Restroke restroke) {
    if (toolBar_ == nullptr) {
        return;
    }

    // The glyphs only, not the tooltips: those are set once when the tools are
    // added and cannot go stale here. A palette change does not reword anything,
    // and a language change is not a thing that happens to a running window --
    // choosing one in preferences asks for a restart.
    if (restroke == Restroke::Yes) {
        for (const ToolbarItem& item : toolbarLayout()) {
            const std::string glyph = commandIcon(item.id);
            toolBar_->SetToolNormalBitmap(item.id, lucideIcon(glyph));
            toolBar_->SetToolDisabledBitmap(item.id, lucideIconDisabled(glyph));
        }
    }

    // Play/Pause carries whichever of the two the transport is asking for, and
    // it is settled here rather than in the loop above, which has just drawn
    // "play" over it from the command table.
    //
    // EVT_UPDATE_UI relabels the *menu* item from state every idle and cannot
    // help here twice over: a wxUpdateUIEvent carries no bitmap, and
    // wxToolBarBase::UpdateWindowUI reads the enabled and checked state off it
    // and drops the text. So the tool is told both directly. That is why the
    // button kept showing a play triangle over a playing track.
    const bool  playing = playback_->playing() && !playback_->paused();
    const char* glyph   = playing ? "pause" : "play";
    toolBar_->SetToolNormalBitmap(PlaybackPlayPause, lucideIcon(glyph));
    toolBar_->SetToolDisabledBitmap(PlaybackPlayPause, lucideIconDisabled(glyph));
    toolBar_->SetToolShortHelp(PlaybackPlayPause, playing ? _("Pause") : _("Play"));
}

void MainFrame::bindCommands() {
    // Both event types, for every command.
    //
    // A menu item, an accelerator and a toolbar tool all raise wxEVT_MENU --
    // wxEVT_TOOL is defined as wxEVT_MENU, not merely handled alongside it -- so
    // the toolbar needs nothing said about it here. A plain wxButton raises
    // wxEVT_BUTTON instead, which is a different event carrying the same id, and
    // binding only the first is why the transport did nothing at all while the
    // menu entries behind it worked, back when it was a row of wxBitmapButtons:
    // a failure with no error attached to it, because the event simply reached
    // the end of the chain unhandled.
    //
    // The second binding is kept now that the transport is a toolbar. Nothing in
    // this window relies on it, but a button anywhere that carries a command id
    // works without discovering this the hard way a second time, and the rule it
    // states -- a command has one handler, whatever surface posts it -- is the
    // point of the file.
    const auto on = [this](CommandId id, auto handler) {
        Bind(wxEVT_MENU, [handler](wxCommandEvent&) { handler(); }, id);
        Bind(wxEVT_BUTTON, [handler](wxCommandEvent&) { handler(); }, id);
    };

    on(FileOpen, [this] { openFiles(); });
    on(FileOpenFolder, [this] { openFolder(); });
    on(FileOpenUrl, [this] { openUrl(); });
    on(FileImportCog, [this] { importFromCog(); });
    on(FileSavePlaylist, [this] { savePlaylistAs(/*selectionOnly=*/false); });
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
            undo_.push(std::make_unique<RandomizeCommand>(
                playlist_, toUtf8(_("Randomize"))));
        }
    });

    on(PlaybackPlayPause, [this] { playback_->playPause(); });
    on(PlaybackStop, [this] { playback_->stop(); });
    on(PlaybackNext, [this] { playback_->next(); });
    on(PlaybackPrevious, [this] { playback_->previous(); });
    on(PlaybackEnqueue, [this] { enqueueSelected(); });

    on(ViewFileTreeRoot, [this] {
        // And open the browser if it was closed. Choosing what to look at and
        // then not being shown it would read as the dialog having done nothing;
        // only on a folder actually chosen, so cancelling opens nothing.
        if (tree_->chooseRootPath()) {
            showFileTree(true);
        }
    });
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
    on(ViewFileTree, [this] { showFileTree(!splitter_->IsSplit()); });

    // --- the playlist's context menu -------------------------------------
    on(PlaylistToggleQueued, [this] { toggleQueuedSelected(); });
    on(PlaylistStopAfter, [this] { toggleStopAfterSelected(); });
    on(PlaylistSaveSelection, [this] { savePlaylistAs(/*selectionOnly=*/true); });
    on(PlaylistSearchArtist, [this] { searchForSelected(/*byAlbum=*/false); });
    on(PlaylistSearchAlbum, [this] { searchForSelected(/*byAlbum=*/true); });
    on(PlaylistReloadInfo, [this] { reloadSelectedInfo(); });
    on(PlaylistResetPlayCount, [this] { resetPlayCountSelected(); });
    on(PlaylistRemoveRating, [this] { removeRatingSelected(); });
    on(PlaylistReveal, [this] { revealSelected(); });
    on(PlaylistTrash, [this] { trashSelected(); });

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
        // The command's own text is already translated -- it is written by
        // whichever handler pushed the command -- so this only has to place it.
        const wxString label =
            undo_.canUndo() ? wxString::Format(_("&Undo %s"), toWx(undo_.undoText()))
                            : wxString(_("&Undo"));
        event.SetText(label + "\tCtrl+Z");
    });
    update(EditRedo, [this](wxUpdateUIEvent& event) {
        event.Enable(undo_.canRedo());
        const wxString label =
            undo_.canRedo() ? wxString::Format(_("&Redo %s"), toWx(undo_.redoText()))
                            : wxString(_("&Redo"));
        event.SetText(label + "\tCtrl+Y");
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
        event.SetText(playing ? _("&Pause") : _("&Play"));
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

    // --- the playlist's context menu -------------------------------------
    //
    // Cog disables the whole menu when nothing is selected
    // (PlaylistView.m:294-300, by walking it and switching every item off).
    // Here each command answers for itself, which is the same result reached
    // from the id -- and it survives the menu being rebuilt, which Cog's does
    // not.
    const auto needsSelection = [this](wxUpdateUIEvent& event) {
        event.Enable(list_->GetSelectedItemsCount() > 0);
    };
    update(PlaylistStopAfter, needsSelection);
    update(PlaylistSaveSelection, needsSelection);
    update(PlaylistReloadInfo, needsSelection);
    update(PlaylistResetPlayCount, needsSelection);
    update(PlaylistRemoveRating, needsSelection);

    update(PlaylistToggleQueued, [this](wxUpdateUIEvent& event) {
        const std::vector<TrackId> ids = selectedTracks();
        event.Enable(!ids.empty());

        // Cog's ToggleQueueTitleTransformer, without the transformer: the label
        // says what the command will do, and says "toggle" when the selection is
        // mixed because that is the honest answer -- each row flips on its own.
        std::size_t queued = 0;
        for (const TrackId id : ids) {
            const PlaylistEntry* entry = playlist_.find(id);
            if (entry != nullptr && entry->queued()) {
                ++queued;
            }
        }
        if (ids.empty() || queued == 0) {
            event.SetText(_("Add to &Queue"));
        } else if (queued == ids.size()) {
            event.SetText(_("Remove from &Queue"));
        } else {
            event.SetText(_("&Toggle Queued"));
        }
    });

    // Cog binds these two to selection.artist and selection.album being non-nil.
    // A track with no artist tag has nothing to search for, and an item that
    // filters the list down to the empty string is worse than one that is greyed.
    const auto searchable = [this](bool byAlbum) {
        return [this, byAlbum](wxUpdateUIEvent& event) {
            const std::vector<TrackId> ids = selectedTracksInOrder();
            const PlaylistEntry* entry = ids.empty() ? nullptr : playlist_.find(ids.front());
            event.Enable(entry != nullptr &&
                         !(byAlbum ? entry->album : entry->artist).empty());
        };
    };
    update(PlaylistSearchArtist, searchable(/*byAlbum=*/false));
    update(PlaylistSearchAlbum, searchable(/*byAlbum=*/true));

    // Both need a file. A selection of internet streams has no folder to open
    // and nothing to move to a trash, and greying them says so before the click
    // rather than in the status line after it.
    const auto needsFiles = [this](wxUpdateUIEvent& event) {
        event.Enable(!selectedPaths().empty());
    };
    update(PlaylistReveal, needsFiles);
    update(PlaylistTrash, needsFiles);
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
        _("Audio Files") + "|" + toWx(patterns) + "|" + _("All Files") + "|*.*";

    wxFileDialog dialog(this, _("Open Files"), wxEmptyString, wxEmptyString, wildcard,
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
        wxDirSelector(_("Open Folder"), wxEmptyString, wxDD_DEFAULT_STYLE,
                      wxDefaultPosition, this);
    if (chosen.IsEmpty()) {
        return;
    }
    addUrls({Url::fromLocalPath(std::filesystem::path{chosen.ToStdWstring()})});
}

void MainFrame::savePlaylistAs(bool selectionOnly) {
    // What is written, decided before the dialog opens: there is no point asking
    // for a filename for an empty selection.
    //
    // Either way the order is the view's rather than the playlist's -- the sort
    // the listener applied, and only the rows the filter leaves. Sorting here is
    // display-only and never reaches Playlist, so playlist_.entries() is still in
    // the order the tracks were added. A listener who sorts by album and saves
    // wants the file in album order; before this, they got the file in the order
    // they had happened to drop folders onto the window.
    std::vector<PlaylistEntry> entries;
    if (selectionOnly) {
        const std::vector<TrackId> ids = selectedTracksInOrder();
        entries.reserve(ids.size());
        for (const TrackId id : ids) {
            if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
                entries.push_back(*entry);
            }
        }
        if (entries.empty()) {
            return;
        }
    } else {
        entries = view_.visibleEntries();
    }

    // The extensions stay inside the descriptions rather than being formatted
    // in: a translator moving "(*.m3u8)" is harmless, and building the string
    // from parts to keep it out of their hands would make the row unreadable in
    // the .po for no gain.
    // A different default name for a selection, so two saves in a row do not
    // offer to overwrite each other by accident.
    wxFileDialog dialog(this, _("Save Playlist"), wxEmptyString,
                        selectionOnly ? "selection.m3u8" : "playlist.m3u8",
                        _("M3U Playlist (*.m3u8)") + "|*.m3u8|" +
                            _("PLS Playlist (*.pls)") + "|*.pls|" +
                            _("XSPF Playlist (*.xspf)") + "|*.xspf",
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
    //
    // Positions in the list being written, not rows of the playlist: the file's
    // queue points into the list the file itself holds. A queued track the filter
    // hid, or that the selection left out, is not in that list, so it is dropped
    // rather than left naming some other row.
    std::unordered_map<TrackId, std::size_t> written;
    written.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        written.emplace(entries[i].id, i);
    }

    std::vector<std::size_t> queuePositions;
    queuePositions.reserve(playlist_.queue().size());
    for (const TrackId id : playlist_.queue()) {
        if (const auto found = written.find(id); found != written.end()) {
            queuePositions.push_back(found->second);
        }
    }

    // PlaylistFile writes text and the caller does the file I/O, which is what
    // keeps it testable without a filesystem. The destination goes in because
    // relative paths are written against it -- which is what makes a playlist
    // survive moving a music folder wholesale.
    const std::string text =
        writePlaylist(format, entries, queuePositions, Url::fromLocalPath(path));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        wxMessageBox(_("Could not write the playlist."), "XPCog",
                     wxOK | wxICON_WARNING, this);
        return;
    }
    // A selection save says how many it wrote, because that is the number the
    // listener is checking. A whole-playlist save says so only when the filter
    // left tracks out -- writing what is shown is the point of the ordering
    // above, but it is also the one way this command can quietly write fewer
    // tracks than the listener thinks it has, and a status line is cheaper than
    // a dialog they would learn to dismiss.
    const std::size_t total = playlist_.size();
    if (selectionOnly) {
        setStatusText(wxString::Format(
            wxPLURAL("Playlist saved: %zu selected track.",
                     "Playlist saved: %zu selected tracks.",
                     static_cast<unsigned>(entries.size())),
            entries.size()));
    } else if (entries.size() < total) {
        setStatusText(wxString::Format(
            _("Playlist saved: %zu of %zu tracks, the rest hidden by the filter."),
            entries.size(), total));
    } else {
        setStatusText(_("Playlist saved."));
    }
}

void MainFrame::addUrls(const std::vector<Url>& urls, int atRow) {
    if (urls.empty()) {
        return;
    }
    pendingScans_.push_back(ScanRequest{urls, atRow, /*reload=*/false, {}});
    pumpScanQueue();
}

// --- importing a Cog library ----------------------------------------------

void MainFrame::importFromCog() {
    // A picker rather than looking in ~/Library/Application Support/Cog, and that
    // is the primary path rather than a fallback. An import is only worth having
    // on the machine somebody is moving *to*, which is a PC as often as not, and
    // there the store arrived by being copied across. Finding it automatically is
    // a convenience for the one platform Cog runs on, and belongs on top of this
    // rather than in place of it.
    wxFileDialog picker(this, _("Open a Cog library"), wxEmptyString,
                        "DataModel.sqlite",
                        _("Cog library (DataModel.sqlite)") + "|DataModel.sqlite|" +
                            _("SQLite databases (*.sqlite)") + "|*.sqlite|" +
                            _("All Files") + "|*.*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (picker.ShowModal() != wxID_OK) {
        return;
    }

    const std::filesystem::path store = pathFromUtf8(picker.GetPath().utf8_string());

    const std::optional<CogLibrary> library = readCogLibrary(store);
    if (!library) {
        wxMessageBox(_("That file could not be read as a Cog library.\n\n"
                       "Cog's is DataModel.sqlite, under Application Support/Cog. "
                       "If Cog is running, copy it along with its -wal and -shm "
                       "files, or the most recent tracks will be missing."),
                     _("Import from Cog"), wxOK | wxICON_WARNING, this);
        return;
    }

    if (library->entries.empty()) {
        // A valid answer, and a different one from the file not opening.
        wxMessageBox(_("That Cog library has no playlist entries in it."),
                     _("Import from Cog"), wxOK | wxICON_INFORMATION, this);
        return;
    }

    CogPlaylistImport imported = cogLibraryToPlaylist(*library);

    std::vector<Url> urls;
    urls.reserve(imported.entries.size());
    for (const PlaylistEntry& entry : imported.entries) {
        urls.push_back(entry.url);
    }

    // Kept alive for the decorator, which runs when the scan finishes. Both are
    // shared rather than captured by reference: this function has returned long
    // before the lambda is called.
    auto fromStore = std::make_shared<std::vector<PlaylistEntry>>(
        std::move(imported.entries));
    auto counts = std::make_shared<CogPlayCounts>(library->playCounts);

    ScanRequest request;
    request.inputs = std::move(urls);
    request.atRow  = -1;
    request.decorate = [this, fromStore, counts](std::vector<PlaylistEntry>& entries) {
        // The merge rule itself is core's, with tests on it. It is the part of
        // this most likely to be subtly wrong -- the failure mode of merging in
        // the wrong direction is a plausible ReplayGain on the wrong track,
        // which nothing complains about.
        static_cast<void>(mergeCogStoreData(*fromStore, entries));

        // After the scan, which is not a preference: the title and artist a play
        // count is matched on are the ones the scanner read from the file, since
        // this import deliberately never opened Cog's metadata blob.
        const CogPlayCountReport matched = applyCogPlayCounts(*counts, entries);

        // Written to the library as well as to the rows, or the counts would
        // last only as long as this playlist.
        if (library_) {
            for (const PlaylistEntry& entry : entries) {
                if (entry.playCount > 0) {
                    static_cast<void>(library_->saveEntry(entry));
                }
            }
        }

        cogImportSummary_ = matched;
    };

    // Said before the scan starts, because the scan is the slow part and a
    // window that does nothing for a minute has not told anybody it is working.
    wxString opening = wxString::Format(
        wxPLURAL("Importing %zu track from Cog...",
                 "Importing %zu tracks from Cog...",
                 static_cast<unsigned>(imported.entries.size())),
        imported.entries.size());
    if (library->prunedDeleted > 0 || library->prunedEmptyUrl > 0 ||
        library->prunedUnparseable > 0) {
        // Cog's own prunes, reported rather than hidden: "900 tracks and this
        // imported 847" is a question somebody will ask.
        const std::size_t dropped = library->prunedDeleted + library->prunedEmptyUrl +
                                    library->prunedUnparseable;
        opening += " ";
        opening += wxString::Format(
            wxPLURAL("(%zu row Cog would not have shown either)",
                     "(%zu rows Cog would not have shown either)",
                     static_cast<unsigned>(dropped)),
            dropped);
    }
    setStatusText(opening);

    cogImportSummary_.reset();
    cogImportFileReferences_ = imported.fileReferences;

    pendingScans_.push_back(std::move(request));
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

    const int  atRow    = request.atRow;
    const bool reload   = request.reload;
    auto       decorate = std::move(request.decorate);
    subscriptions_.push_back(scan_->finished.connect(
        [this, atRow, reload, decorate](const std::vector<PlaylistEntry>& entries,
                                        bool cancelled) {
            // The task owns the thread it is still returning from, so it cannot
            // be destroyed from inside its own callback. Handing it to the event
            // loop to drop is what deleteLater() was doing.
            auto* finished = scan_.release();
            dispatch_([finished] { delete finished; });

            scanBar_->Hide();
            scanCancel_->Hide();
            taskbar_->clearProgress();

            // Copied so the decorator can write to it. The signal hands out
            // a const reference because every other subscriber only reads.
            std::vector<PlaylistEntry> decorated = entries;
            if (decorate) {
                decorate(decorated);
            }
            if (reload) {
                applyReloadedEntries(std::move(decorated));
            } else {
                addScannedEntries(std::move(decorated), atRow, cancelled);
            }
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
        setStatusText(cancelled ? _("Nothing was added.")
                                : _("Nothing playable was found."));
        return;
    }

    // Embedded covers go to the artwork table, which is content-addressed, and
    // leave a hash behind on the entry. Here because this is the first point at
    // which a scanned entry and the library are both in reach.
    //
    // The table exists precisely so an album's twelve tracks hold one copy of
    // their cover between them rather than twelve. Nothing called this, so every
    // cover was persisted the other way instead -- as a blob on the entry's tag
    // rows, once per track. A library with high-resolution art embedded in it
    // reached gigabytes that way, which is most of what made saving and loading
    // the playlist slow.
    if (library_) {
        for (PlaylistEntry& entry : entries) {
            static_cast<void>(library_->adoptArtwork(entry));
        }
    }

    // The row a drop targeted may no longer exist: the scan took time and the
    // user could have edited the playlist meanwhile. insert() clamps, so this
    // lands at the end rather than nowhere.
    const std::size_t where =
        (atRow >= 0) ? static_cast<std::size_t>(atRow) : playlist_.size();
    const std::size_t count = entries.size();

    undo_.push(std::make_unique<InsertTracksCommand>(
        playlist_, where, std::move(entries),
        toUtf8(wxString::Format(wxPLURAL("Add %zu Track", "Add %zu Tracks",
                                         static_cast<unsigned>(count)),
                                count))));

    if (cogImportSummary_) {
        // Said instead of the ordinary summary, because after an import the
        // interesting number is not how long the playlist is now.
        wxString text = wxString::Format(
            wxPLURAL("Imported %zu track from Cog", "Imported %zu tracks from Cog",
                     static_cast<unsigned>(count)),
            count);
        if (cogImportSummary_->matched > 0) {
            text += wxString::Format(_(", %zu with play counts"),
                                     cogImportSummary_->matched);
        }
        if (cogImportFileReferences_ > 0) {
            text += wxString::Format(_("; %zu could not be resolved off a Mac"),
                                     cogImportFileReferences_);
        }
        text += ".";
        setStatusText(text);
        cogImportSummary_.reset();
        cogImportFileReferences_ = 0;
        return;
    }

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

void MainFrame::showSortIndicator() {
    // The control's own arrow cycles ascending, descending, ascending. The view's
    // sort cycles ascending, descending, none -- so from the third click on the
    // two disagree, and the arrow ends up claiming a sort that is not applied and
    // then pointing the wrong way for good. This makes it a readout of the view
    // rather than a state of its own.
    //
    // Both calls go through the port's own wxDataViewColumn, so what is drawn is
    // the native indicator on each platform and not something painted here.
    //
    // The loop index is the display position and the model column is what the
    // view sorts by. They part company the moment a column is dragged, which is
    // why the comparison is against GetModelColumn() and not against `i`.
    for (unsigned int i = 0; i < list_->GetColumnCount(); ++i) {
        wxDataViewColumn* column = list_->GetColumn(i);
        if (static_cast<Column>(column->GetModelColumn()) == view_.sortColumn()) {
            column->SetSortOrder(view_.sortAscending());
        } else if (column->IsSortKey()) {
            // kNoSort is Column::Count, which no column carries, so the
            // no-sort state falls out of this as every column being unset.
            //
            // Guarded, and not defensively: wxDataViewCtrl keeps a list of the
            // columns it is sorting by, and UnsetAsSortKey() on a column that is
            // not in it is a wxFAIL_MSG -- "Column is not used for sorting", a
            // debug alert on top of the playlist. The branch above is what makes
            // that the *normal* path: SetSortOrder() on a single-sort control
            // calls ResetAllSortColumns() first, so by the time this loop reaches
            // the other columns they have already been unset, and every one of
            // them would assert. A release build never noticed, which is why this
            // survived until somebody clicked a header in a debug build.
            column->UnsetAsSortKey();
        }
    }
}

void MainFrame::removeSelected() {
    std::vector<TrackId> ids = selectedTracks();
    if (ids.empty()) {
        return;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        playlist_, std::move(ids),
        toUtf8(wxString::Format(wxPLURAL("Remove %zu Track", "Remove %zu Tracks",
                                         static_cast<unsigned>(count)),
                                count))));
    setStatusText(statusSummary());
}

void MainFrame::enqueueSelected() {
    for (const TrackId id : selectedTracks()) {
        playlist_.enqueue(id);
    }
}

// --- the playlist's context menu -----------------------------------------

std::vector<TrackId> MainFrame::selectedTracksInOrder() const {
    wxDataViewItemArray items;
    list_->GetSelections(items);

    // Through the row numbers rather than through the items, because the order
    // the control reports a selection in is the order it was *made* -- shift-
    // clicking upwards answers bottom to top. Everything on this menu that cares
    // about order wants the order on screen: a playlist saved from a selection,
    // and "the first selected track" for the two searches.
    std::set<unsigned int> rows;
    for (const wxDataViewItem& item : items) {
        rows.insert(model_->GetRow(item));
    }

    std::vector<TrackId> ids;
    ids.reserve(rows.size());
    for (const unsigned int row : rows) {
        if (const TrackId id = view_.trackAt(row); id != kInvalidTrackId) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::vector<std::filesystem::path> MainFrame::selectedPaths() const {
    std::vector<std::filesystem::path> paths;
    for (const TrackId id : selectedTracksInOrder()) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        // Local files only. localPath() answers nullopt for every other scheme,
        // which is what leaves a selection of streams with nothing to act on.
        if (const std::optional<std::filesystem::path> path = entry->url.localPath();
            path.has_value()) {
            paths.push_back(*path);
        }
    }
    return paths;
}

void MainFrame::showPlaylistMenu(const wxDataViewItem& item) {
    if (item.IsOk() && !list_->IsSelected(item)) {
        // Cog's rule (PlaylistView.m:274): right-clicking inside a multiple
        // selection acts on all of it, right-clicking outside one moves the
        // selection to the row under the cursor first.
        list_->UnselectAll();
        list_->Select(item);
        // wx sends no selection-changed event for a programmatic selection, so
        // the two panes that follow it are told by hand. Without this, Info and
        // Lyrics keep describing the row that was selected before the click --
        // which is exactly the row the menu is now not acting on.
        refreshInfo();
        refreshLyrics();
    }

    // Built fresh each time rather than kept, so the labels and the ticks come
    // from the EVT_UPDATE_UI pass PopupMenu() runs before it opens. Held in a
    // unique_ptr because a popup menu belongs to whoever made it -- wx deletes a
    // menu bar's menus and not this one.
    const std::unique_ptr<wxMenu> menu{buildMenu(playlistMenuLayout())};
    PopupMenu(menu.get());
}

void MainFrame::toggleQueuedSelected() {
    // Per entry rather than one decision for the whole selection, which is Cog's
    // -toggleQueuedForEntries: (PlaylistController.m:1909). A mixed selection
    // ends up inverted rather than made uniform, and the menu label says so.
    for (const TrackId id : selectedTracks()) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (entry->queued()) {
            playlist_.dequeue(id);
        } else {
            playlist_.enqueue(id);
        }
    }
    setStatusText(statusSummary());
}

void MainFrame::toggleStopAfterSelected() {
    for (const TrackId id : selectedTracks()) {
        playlist_.update(id,
                         [](PlaylistEntry& entry) { entry.stopAfter = !entry.stopAfter; });
    }
}

void MainFrame::searchForSelected(bool byAlbum) {
    const std::vector<TrackId> ids = selectedTracksInOrder();
    if (ids.empty()) {
        return;
    }
    const PlaylistEntry* entry = playlist_.find(ids.front());
    if (entry == nullptr) {
        return;
    }

    // Cog hands this to its Spotlight window, which is 965 lines of
    // NSMetadataQuery searching the whole disk. The filter box replaced that
    // window -- it searches the playlist, which is where the track came from --
    // so the command fills it in.
    const std::string& text = byAlbum ? entry->album.str() : entry->artist.str();
    if (text.empty()) {
        return;
    }

    // ChangeValue rather than SetValue: SetValue posts wxEVT_TEXT, and the
    // handler on that would set the filter a second time from the string this
    // one just wrote.
    filter_->ChangeValue(toWx(text));
    view_.setFilter(text);
}

void MainFrame::reloadSelectedInfo() {
    std::vector<Url> urls;
    for (const TrackId id : selectedTracksInOrder()) {
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

void MainFrame::applyReloadedEntries(std::vector<PlaylistEntry> entries) {
    if (entries.empty()) {
        setStatusText(_("Nothing could be read."));
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
            const std::int64_t playCount      = entry.playCount;
            const double       position       = entry.currentPosition;
            const bool         stopAfter      = entry.stopAfter;
            const std::int64_t shuffleIndex   = entry.shuffleIndex;
            const std::int32_t queuePosition  = entry.queuePosition;
            const Url          url            = entry.url;

            entry = fresh;

            entry.url           = url;
            entry.playCount     = playCount;
            entry.currentPosition = position;
            entry.stopAfter     = stopAfter;
            entry.shuffleIndex  = shuffleIndex;
            entry.queuePosition = queuePosition;
        });

        if (library_) {
            if (const PlaylistEntry* entry = playlist_.find(found->second);
                entry != nullptr) {
                static_cast<void>(library_->saveEntry(*entry));
            }
        }
        ++updated;
    }

    refreshInfo();
    refreshLyrics();
    setStatusText(wxString::Format(
        wxPLURAL("Re-read %zu track.", "Re-read %zu tracks.",
                 static_cast<unsigned>(updated)),
        updated));
}

void MainFrame::resetPlayCountSelected() {
    std::size_t count = 0;
    for (const TrackId id : selectedTracks()) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (library_) {
            static_cast<void>(library_->resetPlayCount(*entry));
        }
        // And on the entry, which is the copy the Info pane reads. The database
        // alone would leave the old number on screen until the next launch.
        playlist_.update(id, [](PlaylistEntry& target) { target.playCount = 0; });
        ++count;
    }
    refreshInfo();
    setStatusText(wxString::Format(
        wxPLURAL("Play count reset for %zu track.", "Play count reset for %zu tracks.",
                 static_cast<unsigned>(count)),
        count));
}

void MainFrame::removeRatingSelected() {
    if (!library_) {
        return;
    }
    std::size_t count = 0;
    for (const TrackId id : selectedTracks()) {
        if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
            static_cast<void>(library_->setRating(*entry, 0.0F));
            ++count;
        }
    }
    // Said in the status line because there is nowhere else it could show: XPCog
    // has no rating column and no star control, so a rating is something a Cog
    // library brought with it and this is the command that clears it. Silence
    // here would be indistinguishable from the command doing nothing.
    setStatusText(wxString::Format(
        wxPLURAL("Rating removed from %zu track.", "Rating removed from %zu tracks.",
                 static_cast<unsigned>(count)),
        count));
}

void MainFrame::revealSelected() {
    const std::vector<std::filesystem::path> paths = selectedPaths();
    if (paths.empty()) {
        return;
    }
    // The first one, as Cog does (PlaylistController.m:1849). Revealing a
    // multiple selection means a file manager window per folder, which is not
    // what anybody means by "show me where this is".
    if (!platform::revealInFileManager(paths.front())) {
        setStatusText(_("Could not show the file."));
    }
}

void MainFrame::trashSelected() {
    std::vector<TrackId>               ids;
    std::vector<std::filesystem::path> paths;
    for (const TrackId id : selectedTracksInOrder()) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (const std::optional<std::filesystem::path> path = entry->url.localPath();
            path.has_value()) {
            ids.push_back(id);
            paths.push_back(*path);
        }
    }
    if (ids.empty()) {
        return;
    }

    if (!settings_.TrashAskedConsent()) {
        const auto count = static_cast<unsigned>(paths.size());
        wxRichMessageDialog dialog(
            this,
            _("Undo puts the rows back in the playlist. It does not bring the "
              "files back -- restore those from the trash itself."),
            wxString::Format(wxPLURAL("Move %u file to the trash?",
                                      "Move %u files to the trash?", count),
                             count),
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        // Cog's third button, as a checkbox: "yes, and stop asking". A plain yes
        // trashes these files and leaves the question in place.
        dialog.ShowCheckBox(_("Do not ask again"));
        if (dialog.ShowModal() != wxID_YES) {
            return;
        }
        if (dialog.IsCheckBoxChecked()) {
            settings_.setTrashAskedConsent(true);
        }
    }

    std::vector<TrackId> trashed;
    trashed.reserve(ids.size());
    std::size_t failed = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (platform::moveToTrash(paths[i])) {
            trashed.push_back(ids[i]);
        } else {
            // A read-only volume, a network share with no trash, or a file
            // already gone. The row stays, because the file did.
            ++failed;
        }
    }

    if (!trashed.empty()) {
        const std::size_t count = trashed.size();
        undo_.push(std::make_unique<RemoveTracksCommand>(
            playlist_, std::move(trashed),
            toUtf8(wxString::Format(wxPLURAL("Move %zu Track to the Trash",
                                             "Move %zu Tracks to the Trash",
                                             static_cast<unsigned>(count)),
                                    count))));
    }

    if (failed > 0) {
        setStatusText(wxString::Format(
            wxPLURAL("%zu file could not be moved to the trash.",
                     "%zu files could not be moved to the trash.",
                     static_cast<unsigned>(failed)),
            failed));
    } else {
        setStatusText(statusSummary());
    }
}

void MainFrame::onPositionChanged(double seconds, double duration) {
    // The engine's own clock, not `seconds`. This tick is what advances the
    // played-time accumulator, and the two numbers are deliberately different:
    // `seconds` is the playhead, which a seek moves, while playedSeconds() is
    // audio actually delivered to the device, which a seek does not. Feeding the
    // playhead in here would let seeking to the end of a track scrobble it.
    monitor_.advance(playback_->playedSeconds());

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

// --- scrobbling -----------------------------------------------------------

void MainFrame::wireScrobbling() {
    lastFm_ = std::make_unique<LastFmAccount>();

    // Beside the library rather than beside the settings: it is a queue of
    // pending work, not a preference, and it can be deleted without losing
    // anything the listener chose.
    // pathFromUtf8, not the std::filesystem::path constructor: that one reads a
    // std::string through the active code page on Windows, so a listener whose
    // profile is under a name CP-1252 cannot spell would get a queue beside a
    // directory that does not exist. See core/include/xpcog/core/FilePath.hpp,
    // which exists because this went wrong once already.
    const std::filesystem::path queue =
        pathFromUtf8(platform::libraryDatabasePath()).parent_path() /
        "scrobble-queue.json";

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
            setStatusText(
                _("Last.fm access was withdrawn. Reconnect in Preferences."));
        });
    });

    // Sixty seconds, which is Cog's interval for this
    // (OutputNode.m:135-138). Counted for every listener, whether or not they
    // scrobble: this is XPCog's own library, and Library::recordPlay has been
    // written and tested since the library landed with nothing calling it.
    monitor_.onPlayCountReached([this] {
        if (!library_ || currentTrack_ == kInvalidTrackId) {
            return;
        }
        const PlaylistEntry* entry = playlist_.find(currentTrack_);
        if (entry == nullptr) {
            return;
        }
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        static_cast<void>(library_->recordPlay(*entry, now));
    });

    // Half the track or four minutes, whichever came first. The captured
    // pendingScrobble_ is submitted rather than the current entry, for the reason
    // given where it is declared.
    monitor_.onScrobbleReached([this] {
        if (scrobbler_ && !pendingScrobble_.artist.empty()) {
            scrobbler_->submit(pendingScrobble_);
        }
    });
}

void MainFrame::beginScrobbleTrack(TrackId id) {
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
    pendingScrobble_.startedAt =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    monitor_.beginTrack(playback_->playedSeconds(), entry->duration());

    if (scrobbler_) {
        scrobbler_->nowPlaying(pendingScrobble_);
    }
}

void MainFrame::onCurrentTrackChanged(TrackId id) {
    currentTrack_ = id;
    view_.setCurrentTrack(id);

    // Before anything that can fail below it: this is the point the seam reached
    // the speaker, and it is the only moment at which "a new track started" is
    // true exactly once.
    beginScrobbleTrack(id);

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
    //
    // Not translated, and the dash is not decoration: "%s \xE2\x80\x94 XPCog"
    // as a message would invite a translator to reorder it, and a window title
    // that does not start with the track is a taskbar button that says "XPCog"
    // forty times. The separator is a formatting convention rather than
    // language, which is exactly the kind of string a catalogue should not
    // carry.
    SetTitle(text.empty() ? wxString("XPCog") : toWx(text + " \xE2\x80\x94 XPCog"));
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

wxString MainFrame::statusSummary() const {
    double total = 0.0;
    for (const PlaylistEntry& entry : playlist_.entries()) {
        total += entry.duration();
    }
    const std::size_t count = playlist_.size();
    return wxString::Format(trUtf8("%zu track \xE2\x80\x94 %s",
                                     "%zu tracks \xE2\x80\x94 %s",
                                     static_cast<unsigned>(count)),
                            count, toWx(formatClock(total)));
}

void MainFrame::setStatusText(const wxString& text) { SetStatusText(text, 0); }

void MainFrame::applyPaneCaptions() {
    // A table rather than a call beside each AddPane, for the reason the icon
    // table in Commands.cpp gives: this has to be re-applied, so a sweep that
    // walks a list cannot forget a pane where scattered calls will.
    const std::pair<wxWindow*, wxString> captions[] = {
        {spectrum_, _("Spectrum")},
        {equalizer_, _("Equalizer")},
        {info_, _("Info")},
        {lyrics_, _("Lyrics")},
#ifdef XPCOG_HAVE_SC55_PANEL
        {sc55_, _("SC-55 Panel")},
#endif
    };

    for (const auto& [window, caption] : captions) {
        if (window == nullptr) {
            continue;
        }
        if (wxAuiPaneInfo& info = auiManager_.GetPane(window); info.IsOk()) {
            info.Caption(caption);
        }
    }
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
    // The remembered width, read before the browser is opened so it opens at
    // that width rather than at the default and then jumping.
    if (const std::string sash = settings_.rawValue("xpcog.window.sash");
        !sash.empty()) {
        try {
            fileTreeSash_ = std::stoi(sash);
        } catch (const std::exception&) {
            // A value someone edited by hand. The default is fine.
        }
    }
    // Absent means closed, which is what a first launch gets. Only an explicit
    // "1" opens it -- see buildUi() for why that is the default rather than the
    // other way round.
    if (settings_.rawValue("xpcog.window.fileTree") == "1") {
        showFileTree(true);
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
        // The captions came back with it, in whatever language they were saved
        // in. See applyPaneCaptions() for why that is not a cosmetic problem.
        applyPaneCaptions();
        auiManager_.Update();
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
    settings_.setRawValue("xpcog.window.fileTree",
                          splitter_->IsSplit() ? "1" : "0");
    // The live position while it is open, and the remembered one while it is
    // not, so closing the browser does not throw away the width it had.
    const int sash = splitter_->IsSplit() ? splitter_->GetSashPosition() : fileTreeSash_;
    if (sash > 0) {
        settings_.setRawValue("xpcog.window.sash", std::to_string(sash));
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
        setStatusText(wxString::Format(_("Could not save the playlist: %s"),
                                       toWx(library_->lastError())));
    }
    settings_.sync();
}

}  // namespace xpcog::app
