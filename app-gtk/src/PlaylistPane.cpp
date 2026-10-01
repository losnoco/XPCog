#include "PlaylistPane.hpp"

#include "Translations.hpp"

#include <charconv>
#include <filesystem>
#include <string>
#include <string_view>

namespace xpcog::gtk {

namespace {

using Column = PlaylistView::Column;

/// The same key and the same spelling as app-winui/src/PlaylistTable.cpp, so a
/// width dragged in one frontend is the width the other opens with.
constexpr const char* kWidthsKey = "xpcog.playlist.columns";

struct ColumnLayout {
    Column      column;
    const char* key;      ///< in the setting; null for a column never stored
    int         width;    ///< the default, in logical pixels
    float       xalign;
    bool        resizable;
};

/// Title has no key because its width is derived: it takes the slack, which
/// is what PlaylistColumns.hpp explains at length and GTK does with one flag.
constexpr ColumnLayout kLayout[] = {
    {Column::Status, nullptr, 28, 0.5F, false},
    {Column::Track, "track", 44, 1.0F, true},
    {Column::Title, nullptr, 260, 0.0F, true},
    {Column::Artist, "artist", 180, 0.0F, true},
    {Column::Album, "album", 180, 0.0F, true},
    {Column::Length, "length", 64, 1.0F, true},
};

/// The URLs a GdkFileList carries.
std::vector<Url> urlsFrom(const GValue* value) {
    std::vector<Url> urls;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        return urls;
    }
    auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
    for (GSList* node = gdk_file_list_get_files(list); node != nullptr; node = node->next) {
        auto* file = G_FILE(node->data);
        GStr  path(g_file_get_path(file));
        if (path) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path{path.c_str()}));
        } else if (GStr uri(g_file_get_uri(file)); uri) {
            if (std::optional<Url> url = Url::parse(uri.c_str())) {
                urls.push_back(*url);
            }
        }
    }
    return urls;
}

/// The list item a cell widget was made for, stashed on it at setup.
GtkListItem* listItemOf(GtkWidget* cell) {
    return static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(cell), "xpcog-list-item"));
}

/// One cell, from the row: the text, and the two decorations wx's
/// GetAttrByRow draws -- greyed rather than removed when the file could not be
/// opened, since it may come back, and bold when it is the one playing.
void fillCell(GtkLabel* label, XpcogPlaylistRow* row, Column column) {
    gtk_label_set_text(label, xpcog::gtk::playlistRowText(row, column).c_str());

    const PlaylistEntry* entry = xpcog::gtk::playlistRowEntry(row);
    const bool current = xpcog::gtk::playlistRowIsCurrent(row);
    const bool dead    = entry != nullptr && entry->error;
    PangoAttrList* attributes = pango_attr_list_new();
    if (current) {
        pango_attr_list_insert(attributes, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    }
    gtk_label_set_attributes(label, attributes);
    pango_attr_list_unref(attributes);
    if (dead) {
        gtk_widget_add_css_class(GTK_WIDGET(label), "dim-label");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(label), "dim-label");
    }
}

}  // namespace

PlaylistPane::PlaylistPane(PlaylistView& view, Settings& settings)
    : view_(view), settings_(settings) {
    model_ = GObjectPtr<XpcogPlaylistModel>::adopt(playlistModelNew(view_));

    // Both constructors take the model with a full reference; ours stay ours.
    selection_ = GObjectPtr<GtkMultiSelection>::adopt(
        gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(model_.get()))));
    list_ = GTK_COLUMN_VIEW(
        gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(selection_.get()))));
    gtk_column_view_set_show_row_separators(list_, FALSE);
    gtk_column_view_set_enable_rubberband(list_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(list_), "data-table");

    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), GTK_WIDGET(list_));
    gtk_widget_set_vexpand(scroller_, TRUE);
    gtk_widget_set_hexpand(scroller_, TRUE);

    buildColumns();
    restoreColumnWidths();

    // --- what the list reports -------------------------------------------------
    connections_.push_back(Connection::to<void(GtkColumnView*, guint)>(
        list_, "activate", [this](GtkColumnView*, guint position) {
            const TrackId id = view_.trackAt(position);
            if (id != kInvalidTrackId) {
                activated.publish(id);
            }
        }));
    connections_.push_back(Connection::to<void(GtkSelectionModel*, guint, guint)>(
        selection_.get(), "selection-changed",
        [this](GtkSelectionModel*, guint, guint) { selectionChanged.publish(); }));

    // The header's sort. GTK cycles ascending, descending, ascending; the
    // third state -- back to playlist order -- is put in by hand, because a
    // sort you cannot get out of is a playlist whose real order you can no
    // longer see.
    connections_.push_back(Connection::to<void(GtkSorter*, GtkSorterChange)>(
        gtk_column_view_get_sorter(list_), "changed",
        [this](GtkSorter*, GtkSorterChange) { onSorterChanged(); }));

    // Files dropped in the empty part of the list: appended. A drop on a row
    // lands on that cell's own target first, with the row.
    GtkDropTarget* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    connections_.push_back(Connection::to<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop", [this](GtkDropTarget*, const GValue* value, double, double) -> gboolean {
            std::vector<Url> urls = urlsFrom(value);
            if (urls.empty()) {
                return FALSE;
            }
            filesDropped.publish(urls, -1);
            return TRUE;
        }));
    gtk_widget_add_controller(GTK_WIDGET(list_), GTK_EVENT_CONTROLLER(target));
}

PlaylistPane::~PlaylistPane() = default;

void PlaylistPane::buildColumns() {
    for (const ColumnLayout& layout : kLayout) {
        const Column column  = layout.column;
        auto*        factory = gtk_signal_list_item_factory_new();

        connections_.push_back(Connection::to<void(GtkSignalListItemFactory*, GObject*)>(
            factory, "setup", [this, layout](GtkSignalListItemFactory*, GObject* object) {
                auto*      item  = GTK_LIST_ITEM(object);
                GtkWidget* label = gtk_label_new(nullptr);
                gtk_label_set_xalign(GTK_LABEL(label), layout.xalign);
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
                gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
                gtk_widget_set_hexpand(label, TRUE);
                if (layout.column == Column::Track || layout.column == Column::Length) {
                    gtk_widget_add_css_class(label, "numeric");
                }
                g_object_set_data(G_OBJECT(label), "xpcog-list-item", item);
                gtk_list_item_set_child(item, label);

                // The right button, for the context menu, with the row known.
                GtkGesture* click = gtk_gesture_click_new();
                gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
                g_signal_connect(
                    click, "pressed",
                    G_CALLBACK(+[](GtkGestureClick* gesture, int, double x, double y,
                                   gpointer self) {
                        GtkWidget*   cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
                        GtkListItem* pressed = listItemOf(cell);
                        if (pressed == nullptr) {
                            return;
                        }
                        static_cast<PlaylistPane*>(self)->onCellPressed(
                            gesture, gtk_list_item_get_position(pressed), x, y);
                    }),
                    this);
                gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));

                // Files dropped on this row go in above it.
                GtkDropTarget* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
                g_signal_connect(
                    target, "drop",
                    G_CALLBACK(+[](GtkDropTarget* t, const GValue* value, double, double,
                                   gpointer self) -> gboolean {
                        GtkWidget*   cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
                        GtkListItem* dropped = listItemOf(cell);
                        std::vector<Url> urls = urlsFrom(value);
                        if (dropped == nullptr || urls.empty()) {
                            return FALSE;
                        }
                        static_cast<PlaylistPane*>(self)->filesDropped.publish(
                            urls, static_cast<int>(gtk_list_item_get_position(dropped)));
                        return TRUE;
                    }),
                    this);
                gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(target));
            }));

        connections_.push_back(Connection::to<void(GtkSignalListItemFactory*, GObject*)>(
            factory, "bind", [column](GtkSignalListItemFactory*, GObject* object) {
                auto* item  = GTK_LIST_ITEM(object);
                auto* label = GTK_LABEL(gtk_list_item_get_child(item));
                auto* row   = XPCOG_PLAYLIST_ROW(gtk_list_item_get_item(item));
                if (row == nullptr) {
                    return;
                }
                fillCell(label, row, column);
                // And again whenever the row says its text moved on. The
                // handler is remembered on the label for unbind to take off.
                const gulong handler = g_signal_connect_data(
                    row, "changed",
                    G_CALLBACK(+[](XpcogPlaylistRow* changed, gpointer data) {
                        auto* target = static_cast<GtkLabel*>(data);
                        fillCell(target, changed,
                                 static_cast<Column>(GPOINTER_TO_INT(
                                     g_object_get_data(G_OBJECT(target), "xpcog-column"))));
                    }),
                    label, nullptr, static_cast<GConnectFlags>(0));
                g_object_set_data(G_OBJECT(label), "xpcog-column", GINT_TO_POINTER(static_cast<int>(column)));
                g_object_set_data(G_OBJECT(label), "xpcog-changed-handler", GSIZE_TO_POINTER(handler));
            }));

        connections_.push_back(Connection::to<void(GtkSignalListItemFactory*, GObject*)>(
            factory, "unbind", [](GtkSignalListItemFactory*, GObject* object) {
                auto* item  = GTK_LIST_ITEM(object);
                auto* label = gtk_list_item_get_child(item);
                auto* row   = gtk_list_item_get_item(item);
                const gulong handler =
                    GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(label), "xpcog-changed-handler"));
                if (row != nullptr && handler != 0 && g_signal_handler_is_connected(row, handler)) {
                    g_signal_handler_disconnect(row, handler);
                }
                g_object_set_data(G_OBJECT(label), "xpcog-changed-handler", nullptr);
            }));

        const std::string heading = app::tr(std::string(PlaylistView::heading(column)).c_str());
        GtkColumnViewColumn* viewColumn = gtk_column_view_column_new(heading.c_str(), factory);
        gtk_column_view_column_set_resizable(viewColumn, layout.resizable ? TRUE : FALSE);
        if (column == Column::Title) {
            // The slack goes here, and this is the whole of what wx's
            // PlaylistColumns.cpp had to fight the toolkit for.
            gtk_column_view_column_set_expand(viewColumn, TRUE);
        } else {
            gtk_column_view_column_set_fixed_width(viewColumn, layout.width);
        }
        // A sorter that never compares: the view does the sorting, and this
        // exists so the header is clickable and draws its arrow.
        if (column != Column::Status) {
            auto sorter = GObjectPtr<GtkSorter>::adopt(GTK_SORTER(gtk_custom_sorter_new(
                [](gconstpointer, gconstpointer, gpointer) -> int { return 0; }, nullptr, nullptr)));
            gtk_column_view_column_set_sorter(viewColumn, sorter.get());
        }
        gtk_column_view_append_column(list_, viewColumn);
        columns_[static_cast<int>(column)] = viewColumn;  // borrowed; the view owns it
        g_object_unref(viewColumn);
    }
}

void PlaylistPane::onSorterChanged() {
    if (settingSort_) {
        return;
    }
    auto* sorter = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(list_));
    GtkColumnViewColumn* clicked = gtk_column_view_sorter_get_primary_sort_column(sorter);
    const GtkSortType    order   = gtk_column_view_sorter_get_primary_sort_order(sorter);

    Column column = PlaylistView::kNoSort;
    for (int i = 0; i < static_cast<int>(Column::Count); ++i) {
        if (columns_[i] == clicked) {
            column = static_cast<Column>(i);
        }
    }
    if (column == PlaylistView::kNoSort) {
        view_.setSort(PlaylistView::kNoSort, true);
        return;
    }

    const bool ascending = order == GTK_SORT_ASCENDING;
    // The third click: GTK is back at ascending on the column the view has
    // descending. That is the way out to playlist order.
    if (column == view_.sortColumn() && !view_.sortAscending() && ascending) {
        view_.setSort(PlaylistView::kNoSort, true);
        settingSort_ = true;
        gtk_column_view_sort_by_column(list_, nullptr, GTK_SORT_ASCENDING);
        settingSort_ = false;
        return;
    }
    view_.setSort(column, ascending);
}

void PlaylistPane::onCellPressed(GtkGestureClick* gesture, guint position, double x, double y) {
    // Cog's rule (PlaylistView.m:274): right-clicking one of five selected
    // rows acts on the five, and right-clicking a sixth acts on the sixth.
    auto* model = GTK_SELECTION_MODEL(selection_.get());
    if (!gtk_selection_model_is_selected(model, position)) {
        gtk_selection_model_select_item(model, position, TRUE);
    }

    // The pointer, in the list's coordinates, for the popover to point at.
    GtkWidget*       cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    graphene_point_t in   = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y));
    graphene_point_t out  = GRAPHENE_POINT_INIT(0, 0);
    if (!gtk_widget_compute_point(cell, GTK_WIDGET(list_), &in, &out)) {
        out = in;
    }
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    contextMenuRequested.publish(static_cast<double>(out.x), static_cast<double>(out.y));
}

std::vector<TrackId> PlaylistPane::selectedTracks() const {
    std::vector<TrackId> ids;
    GtkBitset* set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_.get()));
    GtkBitsetIter iter;
    guint         position = 0;
    // Ascending positions, which is display order already -- what
    // MainFrame::selectedTracksInOrder() has to sort for.
    if (gtk_bitset_iter_init_first(&iter, set, &position)) {
        do {
            const TrackId id = view_.trackAt(position);
            if (id != kInvalidTrackId) {
                ids.push_back(id);
            }
        } while (gtk_bitset_iter_next(&iter, &position));
    }
    gtk_bitset_unref(set);
    return ids;
}

bool PlaylistPane::revealTrack(TrackId id) {
    const std::optional<std::size_t> row = view_.rowForTrack(id);
    if (!row) {
        return false;
    }
    gtk_column_view_scroll_to(list_, static_cast<guint>(*row), nullptr,
                              static_cast<GtkListScrollFlags>(GTK_LIST_SCROLL_SELECT |
                                                              GTK_LIST_SCROLL_FOCUS),
                              nullptr);
    return true;
}

void PlaylistPane::selectAll() {
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(selection_.get()));
}

void PlaylistPane::restoreColumnWidths() {
    const std::string saved = settings_.rawValue(kWidthsKey);
    std::string_view  rest  = saved;
    while (!rest.empty()) {
        const std::size_t      comma = rest.find(',');
        const std::string_view entry = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key    = entry.substr(0, equals);
        const std::string_view digits = entry.substr(equals + 1);
        int                    width  = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), width);
        // Bounded, as the wx side bounds it: a hand-edited value cannot make a
        // column vanish or push the rest off the screen.
        if (error != std::errc{} || end != digits.data() + digits.size() || width < 16 ||
            width > 2000) {
            continue;
        }
        for (const ColumnLayout& layout : kLayout) {
            if (layout.key != nullptr && key == layout.key) {
                gtk_column_view_column_set_fixed_width(columns_[static_cast<int>(layout.column)],
                                                       width);
            }
        }
    }
}

void PlaylistPane::persistColumnWidths() {
    std::string value;
    for (const ColumnLayout& layout : kLayout) {
        if (layout.key == nullptr) {
            continue;
        }
        const int width = gtk_column_view_column_get_fixed_width(columns_[static_cast<int>(layout.column)]);
        if (width <= 0) {
            continue;
        }
        if (!value.empty()) {
            value += ',';
        }
        value += layout.key;
        value += '=';
        value += std::to_string(width);
    }
    settings_.setRawValue(kWidthsKey, value);
}

}  // namespace xpcog::gtk
