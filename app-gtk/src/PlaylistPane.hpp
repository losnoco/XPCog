// The playlist on screen: a GtkColumnView over the model, and everything the
// list does by itself -- the columns and their widths, the header's three-state
// sort, the selection, activation, the context menu and dropped files.
//
// The counterpart of app/src/PlaylistColumns.hpp plus the list-handling half
// of MainFrame. What it does not do is decide anything about the playlist:
// activating a row asks the session to play it, the context menu's commands
// are the window's actions, and a drop hands its files up. It reports through
// signals and answers questions about the selection.

#pragma once

#include "Glib.hpp"
#include "PlaylistModel.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <gtk/gtk.h>

#include <vector>

namespace xpcog::gtk {

class PlaylistPane {
public:
    PlaylistPane(PlaylistView& view, Settings& settings);
    ~PlaylistPane();

    PlaylistPane(const PlaylistPane&)            = delete;
    PlaylistPane& operator=(const PlaylistPane&) = delete;

    /// The scrolled window holding the list, to put in the layout.
    [[nodiscard]] GtkWidget* widget() const { return scroller_; }

    /// The list itself, for a popover to be parented to.
    [[nodiscard]] GtkWidget* listWidget() const { return GTK_WIDGET(list_); }

    /// The selection in the order it is displayed.
    [[nodiscard]] std::vector<TrackId> selectedTracks() const;

    /// Puts `id` on screen and makes it the selection, replacing whatever was
    /// selected. False when the track has no row -- filtered out, which with a
    /// filter in the box is ordinary rather than an error.
    bool revealTrack(TrackId id);

    void selectAll();

    /// Records the column widths under the same key, in the same spelling,
    /// as the wx frontend, so the two agree.
    void persistColumnWidths();

    /// A row was activated: play it.
    Signal<TrackId> activated;
    /// The selection changed.
    Signal<> selectionChanged;
    /// A right-click on a row, after the select-under-pointer rule has been
    /// applied; the window pops its menu at these list coordinates.
    Signal<double, double> contextMenuRequested;
    /// Files dropped on the list, to go in at `row` -- or at the end for -1.
    Signal<std::vector<Url>, int> filesDropped;

private:
    using Column = PlaylistView::Column;

    void buildColumns();
    void restoreColumnWidths();
    void onSorterChanged();
    void onCellPressed(GtkGestureClick* gesture, guint position, double x, double y);

    PlaylistView& view_;
    Settings&     settings_;

    GObjectPtr<XpcogPlaylistModel> model_;
    GObjectPtr<GtkMultiSelection>  selection_;
    GtkColumnView*                 list_     = nullptr;  // owned by the scroller
    GtkWidget*                     scroller_ = nullptr;  // owned by whoever parents it
    GtkColumnViewColumn*           columns_[static_cast<int>(Column::Count)] = {};

    /// Set while this object is telling the column view what the sort is, so
    /// the sorter's answer is not taken as a click.
    bool settingSort_ = false;

    std::vector<Connection> connections_;
};

}  // namespace xpcog::gtk
