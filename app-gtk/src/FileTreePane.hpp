// The folder browser: the counterpart of app/src/FileTree.hpp.
//
// A GtkListView over a GtkTreeListModel whose levels are GtkDirectoryLists,
// each sorted with folders first and filtered to what the registry can decode
// -- so browsing a music folder shows music rather than cover art and log
// files, and a new codec appears here with no edit. The lists are monitored,
// which is the one thing wx's tree could not do: a file added by another
// program appears without collapsing and expanding the folder.
//
// Rooted at one folder, as Cog's is and as wx's RootedDirCtrl had to be made
// to be: the button at the top is labelled with the folder's name, since
// showing a folder's *contents* means the tree never says what it is
// showing, and it opens the chooser.

#pragma once

#include "Glib.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace xpcog::gtk {

class FileTreePane {
public:
    FileTreePane(const PluginRegistry& registry, GtkWindow* parent);
    ~FileTreePane();

    FileTreePane(const FileTreePane&)            = delete;
    FileTreePane& operator=(const FileTreePane&) = delete;

    /// The pane, to put in a sidebar.
    [[nodiscard]] GtkWidget* widget() const { return root_; }

    /// The folder shown at the top. Empty until one is set; persisted by the
    /// window under the key the wx frontend uses.
    [[nodiscard]] const std::string& rootPath() const { return rootPath_; }
    void setRootPath(const std::string& path);

    /// Asks for a new root folder, asynchronously; rootChosen fires when one
    /// is picked, and nothing does on cancel.
    void chooseRootPath();

    /// Double-clicked, or Enter pressed. Folders come through too -- the
    /// scanner expands them, so the tree does not need to know the difference.
    Signal<std::vector<Url>> activated;
    /// A folder was chosen from the button.
    Signal<> rootChosen;

private:
    void rebuild();
    [[nodiscard]] GListModel* listFor(GFile* directory) const;
    [[nodiscard]] bool shows(GFileInfo* info) const;

    const PluginRegistry& registry_;
    GtkWindow*            parent_;

    GtkWidget* root_       = nullptr;  // an AdwToolbarView; owned by its parent
    GtkButton* rootButton_ = nullptr;
    GtkLabel*  rootLabel_  = nullptr;
    GtkWidget* list_       = nullptr;

    GObjectPtr<GtkTreeListModel>   tree_;
    GObjectPtr<GtkMultiSelection>  selection_;

    std::string rootPath_;

    std::vector<Connection> connections_;
};

}  // namespace xpcog::gtk
