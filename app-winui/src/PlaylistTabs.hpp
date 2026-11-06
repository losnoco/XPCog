// The strip of tabs over the playlist, one per playlist: app-gtk/src/
// PlaylistTabs.hpp with a TabView where GTK has an AdwTabBar.
//
// The same arrangement, for the same reason. The list under the tabs is one
// control that PlaylistView repoints, so a TabViewItem has no content, and the
// TabView is used for what it draws and what it reports. Collapsed while there
// is one playlist, which leaves the window as it was before there were several.

#pragma once

#include "WinRT.hpp"

#include "Commands.hpp"  // uicore's table: app::CommandId

#include "xpcog/core/library/PlaylistSet.hpp"

#include <functional>
#include <string>

namespace xpcog::winui {

class PlaylistTabs {
public:
    struct Hooks {
        /// What to call a playlist.
        std::function<std::string(PlaylistSet::Id)> title;
        /// A tab's close button: removes it from the set or does not, and the
        /// tab goes when the next refresh() says so.
        std::function<void(PlaylistSet::Id)> closeRequested;
        /// The commands the tabs reach: New, and from the context menu Rename
        /// and Delete.
        std::function<void(app::CommandId)> run;
    };

    PlaylistTabs(PlaylistSet& playlists, Hooks hooks);

    [[nodiscard]] mux::Controls::TabView element() const { return view_; }

    /// Brings the tabs into line with the set.
    void refresh();

    /// Whether anything is playing, which is when the playing tab says so.
    void setPlaybackActive(bool active);

private:
    [[nodiscard]] mux::Controls::TabViewItem makeTab(PlaylistSet::Id id);
    [[nodiscard]] mux::Controls::MenuFlyout  makeMenu(PlaylistSet::Id id);
    void onItemsChanged();

    PlaylistSet& playlists_;
    Hooks        hooks_;

    mux::Controls::TabView view_{nullptr};

    /// Set while refresh() moves things, so its own moves are not taken for
    /// the listener's.
    bool syncing_ = false;
    bool active_  = false;
};

}  // namespace xpcog::winui
