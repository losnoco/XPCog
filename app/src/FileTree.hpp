// The folder browser. Replaces Cog's FileTree/ (1,431 lines, built on FSEvents
// and a hand-rolled node cache).
//
// Filtered to what the registry can actually decode, so browsing a music folder
// shows music rather than cover art and log files. The filter comes from the
// registry rather than a fixed list, so a new codec appears here with no edit.
//
// The root is a real root, not a selection. wxGenericDirCtrl shows the whole
// filesystem and SetPath() only expands and highlights inside it, so "choose the
// folder to browse" used to leave every other folder on the machine one scroll
// away. RootedDirCtrl below overrides SetupSections(), which is what populates
// the hidden root item, to put exactly one section there.
//
// One thing lost in the move from QFileSystemModel: it watched the filesystem,
// and wxGenericDirCtrl does not. A file added by another program appears only
// when the folder is collapsed and expanded again. wxFileSystemWatcher would
// close that, and is not wired up here -- recorded as a regression in
// docs/WXPORT.md rather than left to be discovered.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"

// wx/defs.h first, and not for tidiness. wx/dirctrl.h guards its entire
// contents with `#if wxUSE_DIRDLG || wxUSE_FILEDLG` but does not include the
// header that defines them, so as the first wx include it expands to nothing
// and wxGenericDirCtrl is simply not declared.
#include <wx/defs.h>

#include <wx/dirctrl.h>
#include <wx/panel.h>

#include <string>
#include <vector>

class wxBitmapButton;

namespace xpcog::app {

/// wxGenericDirCtrl rooted at one folder.
///
/// wxGenericDirCtrl has no API for this: its tree hangs off a hidden root item
/// populated by SetupSections(), which the base class fills with the home
/// directory, the desktop and every mounted volume. Overriding that one virtual
/// is the whole mechanism -- PopulateNode() calls it whenever the hidden root is
/// expanded (src/generic/dirctrlg.cpp:658), so ReCreateTree() rebuilds from the
/// override rather than from the drive list.
class RootedDirCtrl : public wxGenericDirCtrl {
public:
    using wxGenericDirCtrl::wxGenericDirCtrl;

    /// Shows `path` and nothing above it. Empty restores the ordinary
    /// whole-filesystem tree.
    void setRoot(const wxString& path);

    /// The folder the tree is rooted at, which is *not* GetPath(): that is
    /// whatever the reader last clicked, and persisting it would let the saved
    /// root wander off as they browse.
    [[nodiscard]] wxString root() const { return root_; }

    void SetupSections() override;

private:
    wxString root_;
};

class FileTree : public wxPanel {
public:
    FileTree(wxWindow* parent, const PluginRegistry& registry);

    /// The folder shown at the root. Persisted by the window across launches.
    void                      setRootPath(const std::string& path);
    [[nodiscard]] std::string rootPath() const;

    /// Asks for a new root folder. Cog puts this behind a bare "Choose" button
    /// above its outline view (FileTree.xib, -chooseRootFolder:); here it is the
    /// header button, the context menu, and the View menu, since a button
    /// labelled with the folder you are in answers "where am I" at the same time
    /// as offering to change it.
    ///
    /// Answers whether a folder was actually chosen, which is what lets the
    /// window open the browser when it was closed -- picking a folder to browse
    /// and then not being shown it is the one outcome nobody wants -- without
    /// opening it on a cancelled dialog.
    bool chooseRootPath();

    /// Re-strokes the root button's glyph. Called when the system appearance
    /// changes; the icon is stroked in a colour read at the moment it is built.
    void refreshIcons();

    /// Double-clicked, or Enter pressed. Folders come through too -- the scanner
    /// expands them, so the tree does not need to know the difference.
    Signal<std::vector<Url>> activated;

    /// The context menu's "Add to Playlist".
    Signal<std::vector<Url>> addRequested;

private:
    [[nodiscard]] std::vector<Url> selectedUrls() const;

    void updateRootLabel();

    const PluginRegistry& registry_;
    RootedDirCtrl*        tree_ = nullptr;
    /// Shows the current root and opens the chooser. Its text is the folder's
    /// name, which is otherwise invisible: showing a folder's *contents* means
    /// the tree never says what it is showing.
    wxBitmapButton* root_ = nullptr;
    wxWindow*       rootLabel_ = nullptr;
};

}  // namespace xpcog::app
