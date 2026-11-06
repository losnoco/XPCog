#include "PlaylistTabs.hpp"

#include <algorithm>
#include <utility>

namespace xpcog::gtk {

PlaylistTabs::PlaylistTabs(PlaylistSet& playlists, Title title, CloseRequest closeRequest,
                           GMenuModel* menu)
    : playlists_(playlists),
      title_(std::move(title)),
      closeRequest_(std::move(closeRequest)) {
    box_  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    view_ = adw_tab_view_new();
    bar_  = adw_tab_bar_new();
    adw_tab_bar_set_view(bar_, view_);
    adw_tab_bar_set_autohide(bar_, TRUE);
    adw_tab_view_set_menu_model(view_, menu);

    gtk_widget_set_visible(GTK_WIDGET(view_), FALSE);
    gtk_box_append(GTK_BOX(box_), GTK_WIDGET(bar_));
    gtk_box_append(GTK_BOX(box_), GTK_WIDGET(view_));

    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        view_, "notify::selected-page", [this](GObject*, GParamSpec*) {
            if (syncing_) {
                return;
            }
            if (Tab* tab = tabFor(adw_tab_view_get_selected_page(view_))) {
                playlists_.show(tab->id);
            }
        }));

    connections_.push_back(Connection::to<gboolean(AdwTabView*, AdwTabPage*)>(
        view_, "close-page", [this](AdwTabView*, AdwTabPage* page) -> gboolean {
            if (syncing_) {
                adw_tab_view_close_page_finish(view_, page, TRUE);
                return GDK_EVENT_STOP;
            }
            // Answered no, always: the set decides, and refresh() closes the
            // tab when it has. Asking may take a dialog and a while.
            adw_tab_view_close_page_finish(view_, page, FALSE);
            if (Tab* tab = tabFor(page)) {
                closeRequest_(tab->id);
            }
            return GDK_EVENT_STOP;
        }));

    connections_.push_back(Connection::to<void(AdwTabView*, AdwTabPage*, int)>(
        view_, "page-reordered", [this](AdwTabView*, AdwTabPage* page, int position) {
            if (syncing_) {
                return;
            }
            if (Tab* tab = tabFor(page)) {
                static_cast<void>(playlists_.move(tab->id, static_cast<std::size_t>(position)));
            }
        }));

    // The menu acts on what is shown, so the tab it was opened on comes first.
    connections_.push_back(Connection::to<void(AdwTabView*, AdwTabPage*)>(
        view_, "setup-menu", [this](AdwTabView*, AdwTabPage* page) {
            if (page == nullptr) {
                return;
            }
            if (Tab* tab = tabFor(page)) {
                playlists_.show(tab->id);
            }
        }));

    refresh();
}

PlaylistTabs::~PlaylistTabs() { connections_.clear(); }

PlaylistTabs::Tab* PlaylistTabs::tabFor(AdwTabPage* page) {
    const auto found = std::find_if(tabs_.begin(), tabs_.end(),
                                    [page](const Tab& tab) { return tab.page == page; });
    return found != tabs_.end() ? &*found : nullptr;
}

PlaylistTabs::Tab* PlaylistTabs::tabFor(PlaylistSet::Id id) {
    const auto found = std::find_if(tabs_.begin(), tabs_.end(),
                                    [id](const Tab& tab) { return tab.id == id; });
    return found != tabs_.end() ? &*found : nullptr;
}

void PlaylistTabs::refresh() {
    syncing_ = true;

    // New ones first, so the view never passes through having no pages.
    for (std::size_t i = 0; i < playlists_.size(); ++i) {
        const PlaylistSet::Id id = playlists_.at(i).id;
        if (tabFor(id) == nullptr) {
            AdwTabPage* page =
                adw_tab_view_append(view_, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
            tabs_.push_back({page, id});
        }
    }

    for (auto it = tabs_.begin(); it != tabs_.end();) {
        if (playlists_.indexOf(it->id)) {
            ++it;
        } else {
            adw_tab_view_close_page(view_, it->page);
            it = tabs_.erase(it);
        }
    }

    for (std::size_t i = 0; i < playlists_.size(); ++i) {
        const PlaylistSet::Id id  = playlists_.at(i).id;
        Tab*                  tab = tabFor(id);
        adw_tab_view_reorder_page(view_, tab->page, static_cast<int>(i));
        adw_tab_page_set_title(tab->page, title_(id).c_str());

        GIcon* icon = nullptr;
        if (id == playlists_.playingId() && playlists_.playing().current()) {
            icon = g_themed_icon_new("media-playback-start-symbolic");
        }
        adw_tab_page_set_icon(tab->page, icon);
        if (icon != nullptr) {
            g_object_unref(icon);
        }
    }

    if (Tab* shown = tabFor(playlists_.shownId())) {
        adw_tab_view_set_selected_page(view_, shown->page);
    }

    syncing_ = false;
}

}  // namespace xpcog::gtk
