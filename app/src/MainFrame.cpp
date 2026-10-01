#include "MainFrame.hpp"

#include "AboutDialog.hpp"
#include "AppIcon.hpp"
#include "Commands.hpp"
#include "WxMenus.hpp"
#include "EqualizerPanel.hpp"
#include "FileTree.hpp"
#include "InfoPanel.hpp"
#include "LastFmAccount.hpp"
#include "ListenBrainzAccount.hpp"
#include "LyricsPanel.hpp"
#include "MiniFrame.hpp"
#include "OpenUrlDialog.hpp"
#include "OscilloscopePanel.hpp"
#include "PreferencesDialog.hpp"
#include "Sc55Panel.hpp"
#include "SpectrumPanel.hpp"
#include "LucideIcon.hpp"
#include "PlaylistColumns.hpp"
#include "PlaylistDataModel.hpp"
#include "SeekBar.hpp"
#include "RemoteToken.hpp"
#include "SettingEffect.hpp"
#include "SpeedPanel.hpp"
#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/PlaylistFile.hpp"
// The clients themselves, not only the accounts that own them: the scrobblers
// are built over the client each account hands out, and a forward-declared
// class cannot be passed where its base is wanted.
#include "xpcog/core/scrobble/LastFmClient.hpp"
#include "xpcog/core/scrobble/ListenBrainzClient.hpp"
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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
      session_(registry_, settings_, dispatch_),
      playlist_(session_.playlist()),
      view_(session_.view()),
      undo_(session_.undo()),
      commands_(session_.commands()),
      playback_(&session_.playback()) {
    SetIcons(applicationIcons());

    // See MainFrame.hpp: the cadence and the guards live in tick(), so this is
    // only a clock. Bound to this frame, which is the event handler a wxTimer
    // needs and the reason the controller had to own a bare wxEvtHandler before.
    positionTicker_.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { session_.tick(); }, positionTicker_.GetId());
    positionTicker_.Start(PlaybackController::kTickIntervalMs);

    SetMenuBar(buildMenuBar());
    buildUi();

    // The desktop wants the native window handle, and there is none before
    // buildUi(). Under wx the frame's handle exists as soon as it does, which is
    // why the media integration no longer has to go looking for a window and
    // retry until it finds one.
    session_.attachDesktop(GetHandle());

    presence_ = std::make_unique<StatusPresence>(this, dispatch_);

    seekBar_->setWaveformStyle(waveformStyle());
    seekBar_->setWaveformMode(settings_.WaveformSeekBar());

    wireUp();
    restoreState();

    // Anything the session said before there was a status bar to say it on --
    // a library that would not open.
    if (!session_.lastStatus().empty()) {
        setStatusText(toWx(session_.lastStatus()));
    }

    // The playlist, the resumed track and the remote server, in that order,
    // now that everything above is listening.
    session_.start();

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

    // Before the sweep: it holds a signal connection on the list's tree view
    // and undoes it on destruction, which needs the tree view still there.
    columns_.reset();

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
    // and the session are all still valid. The pointers left behind are cleared
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
    scope_     = nullptr;
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
    columns_ = std::make_unique<PlaylistColumns>(*list_, settings_);

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
    info_      = new InfoPanel(dockHost_, session_.library());
    lyrics_    = new LyricsPanel(dockHost_, [this] { return playback_->position(); });
    applyLyricsLookup();
    // Both visualisers read their settings themselves, and write the few
    // their context menus offer.
    spectrum_   = new SpectrumPanel(dockHost_, playback_->tap(), settings_);
    // The same tap, its own cursor into it.
    scope_      = new OscilloscopePanel(dockHost_, playback_->tap(), settings_);
    speedPanel_ = new SpeedPanel(dockHost_, settings_);

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
    // Hidden by default where the spectrum is shown: one visualiser open on
    // first launch says what the bottom of the window is for, and two say the
    // window is busy. A perspective saved before this pane existed leaves it
    // at these defaults, which is what an unknown pane name does.
    auiManager_.AddPane(scope_, wxAuiPaneInfo()
                                    .Name("scope")
                                    .Bottom()
                                    .BestSize(FromDIP(wxSize(400, 140)))
                                    .MinSize(FromDIP(wxSize(120, 60)))
                                    .Hide());

    // Hidden rather than absent, so it keeps a place in the layout to come back
    // to. 31 sliders is a lot of window to open on someone who wanted a music
    // player.
    // Sized from the panel rather than guessed at. Thirty-two columns have a
    // real height -- readout, slider, label, and the footer under them -- and a
    // pane shorter than that clips the labels off the bottom, where there is no
    // vertical scrolling to reach them. The width is asked for generously and
    // scrolls horizontally when it cannot be had.
    //
    // From contentSize() and not GetBestSize(): the panel scrolls sideways, and
    // a wxScrolled that does reports a best width of a scrollbar and nothing
    // else. See the note on contentSize().
    const wxSize equalizerBest = equalizer_->contentSize();
    auiManager_.AddPane(equalizer_, wxAuiPaneInfo()
                                        .Name("equalizer")
                                        .Bottom()
                                        .BestSize(equalizerBest)
                                        .MinSize(FromDIP(240), equalizerBest.GetHeight())
                                        .Hide());

    // Cog reaches pitch and tempo from a toolbar button and a popover; this is
    // a pane instead, and along the bottom with the other wide, short ones. Two
    // sliders and a row of buttons is the same shape as the equaliser, and the
    // reason for a pane rather than a popup is that this is a control someone
    // leaves open while listening rather than one they open, nudge and dismiss.
    //
    // Sized from the panel: the pitch row disappears under varispeed, so the
    // best size is taken while everything is still on screen and the pane keeps
    // the room rather than resizing under the reader mid-session.
    const wxSize speedBest = speedPanel_->GetBestSize();
    auiManager_.AddPane(speedPanel_, wxAuiPaneInfo()
                                         .Name("speed")
                                         .Bottom()
                                         .BestSize(speedBest)
                                         .MinSize(FromDIP(240), speedBest.GetHeight())
                                         .Hide());

    // The minimum is a floor, not a recommendation. It used to be set at the
    // width the panel reads *well* at, which conflated two different jobs: the
    // best size is what the pane opens at and is the opinion about how wide this
    // wants to be, while the minimum is only the point past which dragging
    // stops. Setting the second to the first means somebody who wants the
    // playlist wide and the panels narrow is refused for their own good.
    //
    // Both are now half what they were, and below about 140 DIP Info stops
    // reading well: its labels wrap a word at a time and a path breaks into a
    // line per directory. Nothing clips -- the panel is an HTML page and it wraps
    // -- so that is the point where dragging narrower stops being useful rather
    // than the point where it has to stop.
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
        toolBar_->AddTool(item.id, toWx(commandLabel(item.id)), lucideIcon(glyph),
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

    // --- the session -------------------------------------------------------
    //
    // Everything below the window reports through here. Each is published once
    // the session has done its own part, so the handlers may read its state.
    observe(session_.status, [this](const std::string& text) { setStatusText(toWx(text)); });
    observe(session_.positionChanged,
            [this](double seconds, double duration) { onPositionChanged(seconds, duration); });
    observe(session_.trackChanged,
            [this](TrackId id, const PlaylistEntry* entry, bool looping) {
                onTrackChanged(id, entry, looping);
            });
    observe(session_.playbackStateChanged,
            [this](bool playing, bool paused) { onPlaybackStateChanged(playing, paused); });
    observe(session_.effectApplied,
            [this](Effect effect, const std::string& key) { onEffectApplied(effect, key); });
    observe(session_.revealRequested, [this](TrackId id) { revealTrack(id); });
    observe(session_.announceTrack,
            [this](const std::string& title, const std::string& body,
                   const std::shared_ptr<const std::vector<std::byte>>& cover) {
                showNotification(title, body, cover);
            });
    observe(session_.tracksUpdated, [this] {
        refreshInfo();
        refreshLyrics();
    });
    observe(session_.waveformUpdated,
            [this](const std::shared_ptr<const WaveformSummary>& summary) {
                seekBar_->setWaveform(summary);
                if (mini_ != nullptr) {
                    mini_->setWaveform(summary);
                }
            });

    // The scan's progress bar and its cancel button. A range of zero is a busy
    // indicator, which is the truthful display while the expansion pass is
    // still counting.
    observe(session_.scanStarted, [this] {
        scanBar_->SetRange(0);
        scanBar_->Show();
        scanCancel_->Show();
    });
    observe(session_.scanProgress, [this](int done, int total) {
        if (total > 0) {
            scanBar_->SetRange(total);
            scanBar_->SetValue(done);
        } else {
            scanBar_->Pulse();
        }
    });
    observe(session_.scanFinished, [this] {
        scanBar_->Hide();
        scanCancel_->Hide();
    });

    // MPRIS only, on Linux. The other two platforms never publish these, so there
    // is nothing to guard: a signal that is never sent costs a connection.
    observe(session_.raiseRequested, [this] {
        Iconize(false);
        Show();
        Raise();
    });
    observe(session_.quitRequested, [this] { Close(true); });
    // The desktop's volume control, or the raw row in Advanced: the slider
    // follows, so the panel and the window cannot show different volumes.
    observe(session_.volumeChanged, [this](double gain) {
        volume_->SetValue(static_cast<int>(std::lround(gain * 100.0)));
    });

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
        scope_->setSampleRate(playback_->sampleRate());
        scope_->setActive(paneShown(scope_) && playing && !paused);
    });

    // --- the equaliser ---------------------------------------------------
    observe(spectrum_->settingsRequested,
            [this] { showPreferences(PreferencesPane::Visualizers); });
    observe(scope_->settingsRequested,
            [this] { showPreferences(PreferencesPane::Visualizers); });
    // The visualisers' context menus write settings; the change takes the
    // same road a Preferences change does, and ends back in the panel.
    observe(spectrum_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });
    observe(scope_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });
    observe(speedPanel_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });
    observe(speedPanel_->settingsRequested,
            [this] { showPreferences(PreferencesPane::PitchTempo); });
    observe(equalizer_->settingChanged,
            [this](const std::string& key) { session_.settingChanged(key); });

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

    // The undo stack needs no observer here: the menu labels come from
    // EVT_UPDATE_UI, which asks the stack directly every idle, and the status
    // line's summary is the session's.

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

    // And again once the native control has had its own say, which on GTK is
    // strictly later than the CallAfter above.
    //
    // The three ports arrive here by different routes. The generic control --
    // which is what wxMSW uses -- offers the click to the application first and
    // does nothing more when it is handled, so the arrow is ours alone and
    // always was. macOS decides immediately after the click, which the
    // CallAfter is placed after. GTK sends this event from a *button-press*
    // handler on the header button and then lets GtkTreeView carry on: the
    // release emits the column's "clicked" signal, which asks the model to sort
    // and, through "sort-column-changed", re-derives every column's indicator
    // from what the model now says. That happens after the idle the CallAfter
    // runs in, so on GTK the arrow was GTK's rather than ours from the moment
    // the button came back up.
    //
    // What GTK rewrites it to is decided by what the model says at that moment,
    // and the CallAfter has already told it: wxDataViewColumn::SetSortOrder()
    // writes the column and the order into wxDataViewCtrlInternal, which is what
    // GtkTreeSortable reads back. So GTK treats the state we just set as the one
    // to move on from, and toggles it. On the first two clicks the arrow
    // therefore comes back *reversed* -- ascending drawn over a descending sort
    // -- and on the third our unset left the model saying nothing is sorted, GTK
    // sees a column that is not it, and puts an ascending arrow on a column the
    // playlist is no longer sorted by. There is no third state for it to reach
    // instead: wxWidgets' model reports no default sort function, which is the
    // one that would give it one.
    //
    // The arrow was wrong on every click, then, and not only the last. A
    // reversed one is simply easier to miss than one that will not go away.
    //
    // wxEVT_DATAVIEW_COLUMN_SORTED is sent at the end of that same model call,
    // after the indicators have been rewritten, so this is the last word.
    // Setting the arrows cannot itself re-enter it -- it pokes the columns, not
    // the sortable -- and everywhere else the event does not arrive at all,
    // where re-stating the view's own sort would be a no-op anyway.
    list_->Bind(wxEVT_DATAVIEW_COLUMN_SORTED,
                [this](wxDataViewEvent&) { showSortIndicator(); });

    filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
        view_.setFilter(toUtf8(event.GetString()));
    });
    filter_->Bind(wxEVT_SEARCH_CANCEL, [this](wxCommandEvent&) {
        filter_->Clear();
        view_.setFilter({});
    });

    volume_->Bind(wxEVT_SLIDER, [this](wxCommandEvent& event) {
        session_.setVolume(event.GetInt() / 100.0);
    });

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { session_.cancelScans(); }, kScanCancelId);

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
        if (event.GetPane()->window == scope_) {
            scope_->setActive(false);
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

void MainFrame::onEffectApplied(Effect effect, const std::string& key) {
    // What to do about a key is decided in SettingEffect.cpp rather than here,
    // and the session has already done the half that reaches the engine, the
    // playlist and the services. What is left is what only a window can do.
    (void)key;
    switch (effect) {
        case Effect::EqualizerCurve:
            // A curve that arrived from somewhere other than these sliders --
            // the remote control picking a preset, or genre tracking. The
            // sliders have to show it, or the window disagrees with what is
            // coming out of the speakers.
            if (equalizer_ != nullptr) {
                equalizer_->refresh();
            }
            break;

        case Effect::RefreshSpeed:
            // Either control may have moved these -- the popup on the strip or
            // the preferences pane -- and the other one is showing a stale
            // number until it is told. The engine choice matters too: it is what
            // decides whether the popup shows its "nothing is listening to
            // these" line.
            if (speedPanel_ != nullptr) {
                speedPanel_->refresh();
            }
            break;

        case Effect::WaveformSeekBar:
            applyWaveformSetting();
            break;

        case Effect::MiniFloating:
            if (mini_ != nullptr) {
                mini_->setFloating(settings_.FloatingMiniWindow());
            }
            break;

        case Effect::RefreshSpectrum:
            spectrum_->applySettings(settings_);
            break;

        case Effect::RefreshScope:
            scope_->applySettings(settings_);
            break;

        case Effect::RefreshPanels:
            // The View menu is where this is normally changed, and that path
            // refreshes the panes itself. This is the other one: the generated
            // row in Advanced, which every setting gets. Without it, changing the
            // mode there does nothing visible until the next selection or track
            // change, which reads as the row being inert.
            refreshInfo();
            refreshLyrics();
            break;

        case Effect::OnlineLyrics:
            // The session re-pointed the lookup; the switch and the redraw are
            // this pane's, which is what turns a pane saying "no lyrics" into
            // one that goes and looks.
            applyLyricsLookup();
            refreshLyrics();
            break;

        case Effect::ReloadDsp:
        case Effect::GenreEqualizer:
        case Effect::ReopenOutput:
        case Effect::PlaylistMode:
        case Effect::Volume:
        case Effect::Scrobbler:
        case Effect::CrashReporter:
        case Effect::RestartRemote:
        case Effect::None:
        case Effect::Internal:
            // Nothing a window shows changes. Volume's slider follows
            // Session::volumeChanged rather than this.
            break;
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

bool MainFrame::dockFloatingPanes() {
    wxAuiPaneInfoArray& panes = auiManager_.GetAllPanes();
    bool docked = false;
    for (std::size_t i = 0; i < panes.GetCount(); ++i) {
        wxAuiPaneInfo& info = panes.Item(i);
        if (info.IsFloating()) {
            // Dock() alone, with no Direction() beside it: the pane still
            // carries the dock it was torn from, and overriding that would send
            // a pane the user had moved to the bottom back to the default side.
            info.Dock();
            docked = true;
        }
    }
    // Hidden panes are docked too, and deliberately. A floating pane that is
    // closed is still floating, so leaving it alone would mean re-opening it
    // later and finding it torn off again with no memory of why.
    if (docked) {
        auiManager_.Update();
    }
    return docked;
}

bool MainFrame::anyPaneFloating() {
    wxAuiPaneInfoArray& panes = auiManager_.GetAllPanes();
    for (std::size_t i = 0; i < panes.GetCount(); ++i) {
        if (panes.Item(i).IsFloating()) {
            return true;
        }
    }
    return false;
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
        return session_.currentTrack();
    }

    // Cog's rule, from both of its controllers (InfoWindowController and
    // LyricsWindowController.m:33-43, which observe the selection and the
    // current entry and prefer the selection exactly like this).
    const std::vector<TrackId> selection = selectedTracks();
    return selection.empty() ? session_.currentTrack() : selection.front();
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
    // Timed lyrics are followed only for the track that is playing; for any
    // other the position is some other song's.
    const TrackId id = panelTrackId();
    lyrics_->setTimed(settings_.LyricsSynced());
    lyrics_->showEntry(playlist_.find(id), id != kInvalidTrackId && id == session_.currentTrack());
}

void MainFrame::showNotification(const std::string& title, const std::string& body,
                                 const std::shared_ptr<const std::vector<std::byte>>& cover) {
    // The cover, decoded the same way the info panel decodes it. Cog writes the
    // art to a temp file because UNNotificationAttachment takes a URL
    // (PlaybackEventController.m:190-200); wx takes a wxIcon, so nothing
    // touches the disk here.
    wxIcon icon;
    if (cover && !cover->empty()) {
        wxMemoryInputStream stream(cover->data(), cover->size());
        wxImage             image;
        if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.IsOk()) {
            // Scaled down first. A balloon draws this at icon size, and handing
            // it a 1500-pixel scan means the platform rescales a megabyte of
            // cover art on the interface thread once a track.
            const int side = FromDIP(48);
            image.Rescale(side, side, wxIMAGE_QUALITY_HIGH);
            icon.CopyFromBitmap(wxBitmap(image));
        }
    }
    presence_->notify(title, body, icon);
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
        // A fresh window read the mode from settings; the shape it has to be
        // handed, or it opens mid-track with a plain bar until the next one.
        mini_->setWaveformStyle(waveformStyle());
        mini_->setWaveformMode(settings_.WaveformSeekBar());
        mini_->setWaveform(seekBar_->waveform());
        mini_->setNowPlaying(
            playlist_.find(session_.currentTrack()) != nullptr ? playlist_.find(session_.currentTrack())->title()
                                                     : std::string{},
            playlist_.find(session_.currentTrack()) != nullptr ? playlist_.find(session_.currentTrack())->artist
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

SeekBar::WaveformStyle MainFrame::waveformStyle() const {
    return SeekBar::styleFrom(settings_);
}

void MainFrame::applyWaveformSetting() {
    const bool on = settings_.WaveformSeekBar();

    seekBar_->setWaveformStyle(waveformStyle());
    seekBar_->setWaveformMode(on);
    // The bar's minimum grew or shrank; the transport row and everything under
    // it has to be laid out again for the frame to take it up.
    Layout();
    if (mini_ != nullptr) {
        mini_->setWaveformStyle(waveformStyle());
        mini_->setWaveformMode(on);
    }
    // Asking for the shape, or dropping it, is the session's, from the same
    // setting: see Session::settingChanged.
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

void MainFrame::showPreferences() { showPreferences(std::nullopt); }

void MainFrame::showPreferences(std::optional<PreferencesPane> pane) {
    PreferencesDialog dialog(this, settings_, session_.lastFm(), session_.scrobbler(),
                             session_.listenBrainz(), session_.listenBrainzScrobbler());
    const Subscription subscription = dialog.settingChanged.connect(
        [this](const std::string& key) { session_.settingChanged(key); });
    if (pane) {
        dialog.showPane(*pane);
    }
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
    on(FileSavePlaylist, [this] { savePlaylistAs(/*selectionOnly=*/false); });
    on(FilePreferences, [this] { showPreferences(); });
    on(HelpAbout, [this] { showAbout(); });
    on(FileQuit, [this] {
        quitting_ = true;
        Close(true);
    });

    on(EditUndo, [this] { commands_.undo(); });
    on(EditRedo, [this] { commands_.redo(); });
    on(EditRemove, [this] { removeSelected(); });
    on(EditSelectAll, [this] { list_->SelectAll(); });
    on(EditRandomize, [this] { commands_.randomize(); });
    on(EditScrollToCurrent, [this] {
        // Enabled whenever something is playing, which is not the same as
        // something being *visible*: a filter in the box can hide the playing
        // track, and then this has nothing to scroll to. Saying so is better
        // than a menu item that appears to do nothing -- the track has not
        // stopped, it is only out of view.
        if (!revealTrack(session_.currentTrack())) {
            setStatusText(_("The playing track is hidden by the filter"));
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
    on(ViewSpeed, [this] { togglePane(speedPanel_, !paneShown(speedPanel_)); });
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

    // Written to the setting and redrawn at once; the pane reads the setting
    // on every redraw, so nothing else needs telling.
    on(ViewTimedLyrics, [this] {
        settings_.setLyricsSynced(!settings_.LyricsSynced());
        refreshLyrics();
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
    on(ViewWaveform, [this] {
        settings_.setWaveformSeekBar(!settings_.WaveformSeekBar());
        // Through the session, so the bars redraw *and* the analysis is asked
        // for, the same way the Appearance pane's row does it.
        session_.settingChanged("waveformSeekBar");
    });
    on(ViewSpectrum, [this] {
        const bool showing = !paneShown(spectrum_);
        togglePane(spectrum_, showing);
        // The clock only runs while the pane is both visible and playing: a
        // 4096-point transform sixty times a second for a hidden widget is the
        // cost this guard exists to avoid.
        spectrum_->setActive(showing && playback_->playing() && !playback_->paused());
    });
    on(ViewOscilloscope, [this] {
        const bool showing = !paneShown(scope_);
        togglePane(scope_, showing);
        scope_->setActive(showing && playback_->playing() && !playback_->paused());
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
    on(ViewDockPanes, [this] { dockFloatingPanes(); });
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
    // On whether anything is playing, not on whether it has a row. Cog draws the
    // same line (-validateUserInterfaceItem: refuses only when stopped), and the
    // row question costs a scan of the visible order -- which this would pay on
    // every idle, for a menu nobody has opened.
    update(EditScrollToCurrent, [this](wxUpdateUIEvent& event) {
        event.Enable(session_.currentTrack() != kInvalidTrackId);
    });
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
    update(ViewSpeed,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(speedPanel_)); });
    update(ViewInfo, [this](wxUpdateUIEvent& event) { event.Check(paneShown(info_)); });
    update(ViewLyrics,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(lyrics_)); });
    update(ViewTimedLyrics,
           [this](wxUpdateUIEvent& event) { event.Check(settings_.LyricsSynced()); });
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
    update(ViewOscilloscope,
           [this](wxUpdateUIEvent& event) { event.Check(paneShown(scope_)); });
    update(ViewWaveform,
           [this](wxUpdateUIEvent& event) { event.Check(settings_.WaveformSeekBar()); });
    update(ViewDockPanes,
           [this](wxUpdateUIEvent& event) { event.Enable(anyPaneFloating()); });
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
    // There is no point asking for a filename for an empty selection.
    std::vector<TrackId> selection;
    if (selectionOnly) {
        selection = selectedTracksInOrder();
        if (selection.empty()) {
            return;
        }
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
    if (!session_.savePlaylist(path, selectionOnly ? &selection : nullptr)) {
        wxMessageBox(_("Could not write the playlist."), "XPCog", wxOK | wxICON_WARNING, this);
    }
}

void MainFrame::addUrls(const std::vector<Url>& urls, int atRow) {
    session_.addUrls(urls, atRow);
}

// --- playback -----------------------------------------------------------

void MainFrame::activateRow(unsigned int row) {
    const TrackId id = view_.trackAt(row);
    if (id != kInvalidTrackId) {
        playback_->playTrack(id);
    }
}

bool MainFrame::revealTrack(TrackId id) {
    if (id == kInvalidTrackId) {
        return false;
    }
    // The view's row, not the playlist's index: a sort or a filter is exactly
    // what makes the two differ, and it is exactly when this is worth having.
    const auto row = view_.rowForTrack(id);
    if (!row) {
        return false;
    }

    const wxDataViewItem item = model_->GetItem(static_cast<unsigned>(*row));
    // Unselect first, so this replaces the selection rather than adding to it --
    // Cog's -scrollToCurrentEntry: selects byExtendingSelection:NO. And
    // EnsureVisible as well as Select, because a selection that has scrolled out
    // of sight is one the listener still has to go looking for.
    list_->UnselectAll();
    list_->Select(item);
    list_->EnsureVisible(item);
    return true;
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
    if (commands_.remove(selectedTracks()) > 0) {
        setStatusText(statusSummary());
    }
}

void MainFrame::enqueueSelected() { commands_.setQueued(selectedTracks(), true); }

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
    commands_.toggleQueued(selectedTracks());
    setStatusText(statusSummary());
}

void MainFrame::toggleStopAfterSelected() {
    commands_.toggleStopAfter(selectedTracks());
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

void MainFrame::reloadSelectedInfo() { session_.reloadTracks(selectedTracksInOrder()); }

void MainFrame::resetPlayCountSelected() { session_.resetPlayCount(selectedTracks()); }

void MainFrame::removeRatingSelected() { session_.removeRating(selectedTracks()); }

void MainFrame::revealSelected() { session_.revealInFileManager(selectedTracksInOrder()); }

void MainFrame::trashSelected() {
    std::vector<TrackId> ids;
    std::size_t          files = 0;
    for (const TrackId id : selectedTracksInOrder()) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry != nullptr && entry->url.localPath().has_value()) {
            ids.push_back(id);
            ++files;
        }
    }
    if (ids.empty()) {
        return;
    }

    if (!settings_.TrashAskedConsent()) {
        const auto count = static_cast<unsigned>(files);
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

    session_.trashTracks(ids);
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
}

// --- scrobbling -----------------------------------------------------------

// --- lyrics ---------------------------------------------------------------

void MainFrame::applyLyricsLookup() {
    if (lyrics_ == nullptr) {
        return;
    }
    lyrics_->setLookup(session_.lyricsLookup());
}

void MainFrame::onTrackChanged(TrackId id, const PlaylistEntry* entry, bool looping) {
    (void)id;
    (void)looping;
    const std::string text = entry != nullptr ? entry->display() : std::string{};

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

    refreshInfo();
    refreshLyrics();
}

void MainFrame::onPlaybackStateChanged(bool playing, bool paused) {
    refreshTransportIcons();
    presence_->setPlaybackState(playing, paused);
    if (mini_ != nullptr) {
        mini_->setPlaybackState(playing, paused);
    }

    if (!playing) {
        seekBar_->setDuration(0.0);
        clock_->SetLabelText("0:00 / 0:00");
        // The waveform is not cleared here. A stop announces kInvalidTrackId
        // through the session's trackChanged, which is where the bars are
        // cleared; and this signal also says "not playing" once, briefly, right
        // after a start.
    }
}

// --- state --------------------------------------------------------------

wxString MainFrame::statusSummary() const { return toWx(session_.statusSummary()); }

void MainFrame::setStatusText(const wxString& text) { SetStatusText(text, 0); }

void MainFrame::applyPaneCaptions() {
    // A table rather than a call beside each AddPane, for the reason the icon
    // table in Commands.cpp gives: this has to be re-applied, so a sweep that
    // walks a list cannot forget a pane where scattered calls will.
    const std::pair<wxWindow*, wxString> captions[] = {
        {spectrum_, _("Spectrum")},
        {scope_, _("Oscilloscope")},
        {equalizer_, _("Equalizer")},
        {speedPanel_, _("Pitch & Tempo")},
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

    columns_->restore();

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
    columns_->persist();
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

    // The playlist, the playback position and the sync itself, after the keys
    // above so one sync covers both.
    session_.save();
}


// --- the REST remote control ---------------------------------------------

}  // namespace xpcog::app
