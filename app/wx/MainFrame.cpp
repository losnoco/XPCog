#include "MainFrame.hpp"

#include "AppIcon.hpp"
#include "Text.hpp"

#include "xpcog/platform/SettingsStore.hpp"

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <utility>

namespace xpcog::app {

MainFrame::MainFrame(const PluginRegistry& registry, Settings& settings,
                     Dispatcher dispatch)
    : wxFrame(nullptr, wxID_ANY, "XPCog", wxDefaultPosition, wxSize(1100, 680)),
      registry_(registry),
      settings_(settings),
      dispatch_(std::move(dispatch)),
      view_(playlist_) {
    SetIcons(applicationIcons());

    library_ = std::make_unique<Library>();
    if (!library_->open(platform::libraryDatabasePath())) {
        // A library that will not open is not fatal: the player still plays, it
        // just will not remember the playlist. Saying so once beats failing to
        // launch.
        library_.reset();
    }

    buildUi();

    if (library_ && library_->loadPlaylist(playlist_)) {
        // Restoring the saved playlist is not an edit the user made, so it must
        // not be the first thing Undo offers to take back.
        undo_.clear();
    }
}

MainFrame::~MainFrame() = default;

void MainFrame::buildUi() {
    // Deliberately a placeholder. The playlist, the transport and the docks
    // arrive in the next step; what this commit is proving is that the toolkit
    // links, the resources decode and the frame comes up owning a real library
    // and a real playlist.
    auto* panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(new wxStaticText(panel, wxID_ANY, "XPCog"), 0,
               wxALIGN_CENTER_HORIZONTAL);
    sizer->AddStretchSpacer();
    panel->SetSizer(sizer);
}

void MainFrame::openUrls(const std::vector<Url>& urls) {
    // Arrives in the next step, with the scan task and the undo stack behind it.
    (void)urls;
}

}  // namespace xpcog::app
