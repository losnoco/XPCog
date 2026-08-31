#include "FileTree.hpp"

#include "Commands.hpp"
#include "LucideIcon.hpp"
#include "Text.hpp"

#include "xpcog/core/FilePath.hpp"

#include <wx/bmpbuttn.h>
#include <wx/dirctrl.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
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

void RootedDirCtrl::SetupSections() {
    if (root_.empty()) {
        // No root chosen: the ordinary tree, home and desktop and every volume.
        wxGenericDirCtrl::SetupSections();
        return;
    }

    // The leaf name, so the top of the tree reads "Music" rather than the whole
    // path. A volume root has no leaf, and there the path is the name.
    const wxFileName folder = wxFileName::DirName(root_);
    const wxString   name =
        folder.GetDirs().IsEmpty() ? root_ : folder.GetDirs().Last();

    // Image 1, which is what the base class gives the home directory -- an open
    // folder rather than the drive icon it gives a volume.
    AddSection(root_, name, 1);
}

void RootedDirCtrl::setRoot(const wxString& path) {
    root_ = path;
    // Expanded and selected once the tree is rebuilt: ExpandRoot() calls
    // ExpandPath(m_defaultPath), so this is what stops the new root arriving
    // collapsed and needing a click to show anything.
    SetDefaultPath(path);
    // CollapseDir(root) + ExpandRoot(), and the collapse is what clears
    // m_isExpanded so PopulateNode() calls SetupSections() again rather than
    // returning early.
    ReCreateTree();
}

FileTree::FileTree(wxWindow* parent, const PluginRegistry& registry)
    : wxPanel(parent, wxID_ANY), registry_(registry) {
    root_ = new wxBitmapButton(this, kRootButtonId, lucideIcon("folder-open"));
    root_->SetToolTip(_("Choose the folder to browse"));

    rootLabel_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);

    // The root is set afterwards, never here: SetupSections() is reached from
    // the base class's own constructor, where a virtual call still dispatches to
    // the base. Constructing with the whole filesystem and rerooting once the
    // object is complete is the only order that works.
    tree_ = new RootedDirCtrl(this, kTreeId, wxDirDialogDefaultFolderStr,
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
    //
    // Skipped only when nothing was added. Both tree controls treat an
    // unprocessed activation as permission to toggle the item -- the generic one
    // toggles when ProcessEvent() returns false (wxWidgets/src/generic/
    // treectlg.cpp:3954), and the native Windows one returns `processed` as the
    // notification's result for exactly the same purpose (src/msw/treectrl.cpp:
    // 3780). So skipping meant double-clicking a folder both queued it and
    // opened it, and the tree jumped about under the pointer while the scan ran.
    tree_->GetTreeCtrl()->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& event) {
        std::vector<Url> urls = selectedUrls();
        if (urls.empty()) {
            event.Skip();
            return;
        }
        activated.publish(urls);
    });

    tree_->GetTreeCtrl()->Bind(wxEVT_TREE_ITEM_MENU, [this](wxTreeEvent& event) {
        wxMenu menu;
        menu.Append(kAddToPlaylistId, _("&Add to Playlist"));
        menu.AppendSeparator();
        menu.Append(kChooseRootId, _("Choose &Root Folder..."));
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent& command) {
            if (command.GetId() == kChooseRootId) {
                static_cast<void>(chooseRootPath());
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
    tree_->setRoot(toWx(path));
    updateRootLabel();
}

std::string FileTree::rootPath() const { return toUtf8(tree_->root()); }

bool FileTree::chooseRootPath() {
    const wxString chosen = wxDirSelector(_("Choose the folder to browse"), tree_->root(),
                                          wxDD_DEFAULT_STYLE, wxDefaultPosition, this);
    if (chosen.IsEmpty()) {
        return false;
    }
    tree_->setRoot(chosen);
    updateRootLabel();
    return true;
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
