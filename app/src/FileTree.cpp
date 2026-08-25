#include "FileTree.hpp"

#include "Commands.hpp"
#include "LucideIcon.hpp"
#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"

#include <wx/bmpbuttn.h>
#include <wx/dirctrl.h>
#include <wx/dirdlg.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/translation.h>
#include <wx/treectrl.h>

#include <filesystem>
#include <string>

namespace xpcog::app {
namespace {

/// Ids for this panel's own widgets, above everything Commands.hpp claims.
enum : int {
    kRootButtonId = FirstWidgetId + 10,
    kTreeId,
    kAddToPlaylistId,
    kChooseRootId,
};

/// `"Audio Files|*.flac;*.mp3;...|All Files|*.*"`.
///
/// Built from the registry rather than written out, so a new codec appears here
/// with no edit -- a hand-written list would be wrong the first time a decoder
/// was added and would stay wrong quietly.
[[nodiscard]] wxString buildFilter(const PluginRegistry& registry) {
    std::string patterns;
    for (const std::string& extension : registry.allExtensions()) {
        if (!patterns.empty()) {
            patterns += ';';
        }
        patterns += "*." + extension;
    }
    // The two descriptions are translated and the patterns are not, which is
    // the split every file dialog on every platform wants: `*.flac` is not
    // language, and a translator handed the whole string could break the filter
    // by tidying a semicolon.
    if (patterns.empty()) {
        return _("All Files") + "|*.*";
    }
    return _("Audio Files") + "|" + toWx(patterns) + "|" + _("All Files") + "|*.*";
}

}  // namespace

FileTree::FileTree(wxWindow* parent, const PluginRegistry& registry)
    : wxPanel(parent, wxID_ANY), registry_(registry) {
    root_ = new wxBitmapButton(this, kRootButtonId, lucideIcon("folder-open"));
    root_->SetToolTip(_("Choose the folder to browse"));

    rootLabel_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);

    tree_ = new wxGenericDirCtrl(this, kTreeId, wxDirDialogDefaultFolderStr,
                                 wxDefaultPosition, wxDefaultSize,
                                 wxDIRCTRL_3D_INTERNAL | wxDIRCTRL_MULTIPLE,
                                 buildFilter(registry_));

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(root_, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(2));
    header->Add(rootLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(header, 0, wxEXPAND);
    sizer->Add(tree_, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { chooseRootPath(); }, kRootButtonId);

    // Double-click or Enter. wxGenericDirCtrl reports both as ITEM_ACTIVATED on
    // the tree it wraps.
    tree_->GetTreeCtrl()->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& event) {
        event.Skip();
        if (std::vector<Url> urls = selectedUrls(); !urls.empty()) {
            activated.publish(urls);
        }
    });

    tree_->GetTreeCtrl()->Bind(wxEVT_TREE_ITEM_MENU, [this](wxTreeEvent& event) {
        wxMenu menu;
        menu.Append(kAddToPlaylistId, _("&Add to Playlist"));
        menu.AppendSeparator();
        menu.Append(kChooseRootId, _("Choose &Root Folder..."));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent& command) {
            if (command.GetId() == kChooseRootId) {
                chooseRootPath();
                return;
            }
            if (std::vector<Url> urls = selectedUrls(); !urls.empty()) {
                addRequested.publish(urls);
            }
        });
        PopupMenu(&menu);
        event.Skip(false);
    });

    updateRootLabel();
}

void FileTree::setRootPath(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(pathFromUtf8(path), ec)) {
        // A root saved from a removable drive, or one since renamed. Keeping
        // whatever is currently shown beats emptying the tree with no
        // explanation.
        return;
    }
    tree_->SetPath(toWx(path));
    updateRootLabel();
}

std::string FileTree::rootPath() const { return toUtf8(tree_->GetPath()); }

void FileTree::chooseRootPath() {
    const wxString chosen = wxDirSelector(_("Choose the folder to browse"), tree_->GetPath(),
                                          wxDD_DEFAULT_STYLE, wxDefaultPosition, this);
    if (chosen.IsEmpty()) {
        return;
    }
    tree_->SetPath(chosen);
    updateRootLabel();
}

void FileTree::refreshIcons() {
    if (root_ != nullptr) {
        root_->SetBitmap(lucideIcon("folder-open"));
    }
}

void FileTree::updateRootLabel() {
    const std::filesystem::path path = pathFromUtf8(rootPath());
    // The leaf name, or the whole thing when there is no leaf -- a drive root,
    // where filename() is empty and the path itself is the name.
    const std::string name =
        path.filename().empty() ? path.string() : path.filename().string();
    static_cast<wxStaticText*>(rootLabel_)->SetLabelText(toWx(name));
    Layout();
}

std::vector<Url> FileTree::selectedUrls() const {
    wxArrayString paths;
    tree_->GetPaths(paths);

    std::vector<Url> urls;
    urls.reserve(paths.GetCount());
    for (const wxString& path : paths) {
        urls.push_back(Url::fromLocalPath(std::filesystem::path{path.ToStdWstring()}));
    }
    return urls;
}

}  // namespace xpcog::app
