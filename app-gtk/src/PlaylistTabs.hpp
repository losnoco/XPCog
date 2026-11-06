// The strip of tabs over the playlist, one per playlist.
//
// An AdwTabBar wants an AdwTabView, and an AdwTabView wants a child per page,
// but the list under the tabs is one widget that PlaylistView repoints: a list
// per tab would mean a model and a column set per tab to keep in step. So each
// page's child is an empty box, the view stays hidden, and the bar is used for
// what it draws and what it reports. It hides itself while there is one
// playlist, which leaves the window as it was before there were several.

#pragma once

#include "Glib.hpp"

#include "xpcog/core/library/PlaylistSet.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace xpcog::gtk {

class PlaylistTabs {
public:
    using Title        = std::function<std::string(PlaylistSet::Id)>;
    using CloseRequest = std::function<void(PlaylistSet::Id)>;

    /// `title` names a playlist. `closeRequest` is asked when a tab's close
    /// button is pressed, and removes it from the set or does not: the tab
    /// goes when the next refresh() says so. `menu` is the tabs' context menu.
    PlaylistTabs(PlaylistSet& playlists, Title title, CloseRequest closeRequest,
                 GMenuModel* menu);
    ~PlaylistTabs();

    PlaylistTabs(const PlaylistTabs&)            = delete;
    PlaylistTabs& operator=(const PlaylistTabs&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept { return box_; }

    /// Brings the tabs into line with the set.
    void refresh();

    /// Whether anything is playing, which is when the playing tab says so.
    void setPlaybackActive(bool active);

private:
    struct Tab {
        AdwTabPage*     page = nullptr;
        PlaylistSet::Id id   = 0;
    };

    [[nodiscard]] Tab* tabFor(AdwTabPage* page);
    [[nodiscard]] Tab* tabFor(PlaylistSet::Id id);

    PlaylistSet& playlists_;
    Title        title_;
    CloseRequest closeRequest_;

    GtkWidget*  box_  = nullptr;
    AdwTabView* view_ = nullptr;
    AdwTabBar*  bar_  = nullptr;

    std::vector<Tab>        tabs_;
    std::vector<Connection> connections_;

    /// Set while refresh() moves things, so its own moves are not taken for
    /// the listener's.
    bool syncing_ = false;
    bool active_  = false;
};

}  // namespace xpcog::gtk
