// The player window.
//
// The wxWidgets counterpart of the Qt MainWindow, and it keeps that file's one
// useful convention: every connection this window makes lives in wireUp(). That
// was the answer to Cog's 190 bindings scattered across a dozen XIBs -- when the
// question is "what updates when the track changes", there is exactly one place
// to look -- and it is worth as much here.
//
// Two lifetime rules, both of which the toolkit used to enforce and no longer
// does:
//
// **Subscriptions are held.** Qt disconnected automatically when either end was
// destroyed. xpcog::Signal hands back an RAII token instead, and letting go of it
// is what disconnects, so every connection is kept in `subscriptions_` -- which is
// declared last, so it is destroyed first, before the objects whose signals it
// holds. A handler firing into a half-destroyed window is the failure that
// ordering exists to make impossible.
//
// **Commands are integers.** wx has no QAction, so a command is an id that a menu
// item, a toolbar button and an accelerator all post. Enabled state and labels
// come from EVT_UPDATE_UI rather than from a shared object, which means the menu
// bar, the playlist's context menu and the tray menu stay in step without anyone
// remembering to update three of them.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/ScanTask.hpp"

#include <wx/frame.h>

#include <functional>
#include <memory>
#include <vector>

namespace xpcog::app {

using Dispatcher = std::function<void(std::function<void()>)>;

class MainFrame : public wxFrame {
public:
    MainFrame(const PluginRegistry& registry, Settings& settings, Dispatcher dispatch);
    ~MainFrame() override;

    /// Adds files, folders, playlists or cue sheets. Public so the command line
    /// and the OS's open-document event reach the same code path the menu does.
    void openUrls(const std::vector<Url>& urls);

private:
    void buildUi();

    const PluginRegistry& registry_;
    Settings&             settings_;
    Dispatcher            dispatch_;

    Playlist                 playlist_;
    PluginCache              cache_;
    std::unique_ptr<Library> library_;
    PlaylistView             view_;
    UndoStack                undo_;

    /// Declared last; see the class comment. Members are destroyed in reverse
    /// order, so this goes first.
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::app
