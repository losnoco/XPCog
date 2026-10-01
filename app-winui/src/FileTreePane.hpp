// The folder browser: the counterpart of app-gtk/src/FileTreePane.hpp.
//
// A TreeView rooted at one folder, as Cog's is and as the GTK one is: the
// button at the top is labelled with the folder's name -- showing a folder's
// *contents* means the tree never says what it is showing -- and it opens the
// chooser. Folders list their children only when they are first expanded, and
// only folders and files the registry can decode are listed, so browsing a
// music folder shows music rather than cover art and log files, and a new
// codec appears here with no edit.
//
// Unlike GTK's, the listing is not monitored: a file added by another program
// appears the next time its folder is listed, as in wx's tree.

#pragma once

#include "WinRT.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::winui {

class FileTreePane {
public:
    /// `windowId` names the window the folder chooser belongs to; asked for
    /// when the chooser opens, since the pane is built before the window has
    /// one to give.
    FileTreePane(const PluginRegistry& registry,
                 std::function<winrt::Microsoft::UI::WindowId()> windowId);
    ~FileTreePane();

    FileTreePane(const FileTreePane&)            = delete;
    FileTreePane& operator=(const FileTreePane&) = delete;

    /// The pane, to put in a sidebar. Transparent: the host gives it its surface.
    [[nodiscard]] mux::UIElement element() const;

    /// The folder shown at the top. Empty until one is set; persisted by the
    /// window under the key the other frontends use, xpcog.fileTree.root.
    [[nodiscard]] const std::string& rootPath() const;
    /// Ignored for a path that is not a folder now -- a root saved from a
    /// removable drive, or one since renamed: keeping what is shown beats
    /// emptying the tree with no explanation.
    void setRootPath(const std::string& path);

    /// Asks for a new root folder; rootChosen fires when one is picked, and
    /// nothing does on cancel.
    void chooseRootPath();

    /// Double-clicked, or Enter pressed. Folders come through too -- the
    /// scanner expands them, so the tree does not need to know the difference.
    Signal<std::vector<Url>> activated;
    /// "Add to Playlist" from the context menu. Separate from activated for the
    /// same reason as in wx: the window may one day treat them differently.
    Signal<std::vector<Url>> addRequested;
    /// A folder was chosen from the button or the context menu.
    Signal<> rootChosen;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xpcog::winui
