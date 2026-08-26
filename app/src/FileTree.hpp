// The folder browser. Replaces Cog's FileTree/ (1,431 lines, built on FSEvents
// and a hand-rolled node cache).
//
// Filtered to what the registry can actually decode, so browsing a music folder
// shows music rather than cover art and log files. The filter comes from the
// registry rather than a fixed list, so a new codec appears here with no edit.
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

#include <wx/panel.h>

#include <string>
#include <vector>

class wxBitmapButton;
class wxGenericDirCtrl;

namespace xpcog::app {

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
    wxGenericDirCtrl*     tree_ = nullptr;
    /// Shows the current root and opens the chooser. Its text is the folder's
    /// name, which is otherwise invisible: showing a folder's *contents* means
    /// the tree never says what it is showing.
    wxBitmapButton* root_ = nullptr;
    wxWindow*       rootLabel_ = nullptr;
};

}  // namespace xpcog::app
