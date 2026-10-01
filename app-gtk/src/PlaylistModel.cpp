#include "PlaylistModel.hpp"

#include "Glib.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

using xpcog::PlaylistEntry;
using xpcog::PlaylistView;
using xpcog::TrackId;

// --- the row --------------------------------------------------------------------

struct _XpcogPlaylistRow {
    GObject parent_instance;

    PlaylistView* view;
    TrackId       id;
    guint         index;
};

G_DEFINE_FINAL_TYPE(XpcogPlaylistRow, xpcog_playlist_row, G_TYPE_OBJECT)

namespace {
guint rowChangedSignal = 0;
}

static void xpcog_playlist_row_class_init(XpcogPlaylistRowClass* klass) {
    // "changed": the text behind this row is not what it was. A bound cell
    // re-reads on it; see PlaylistPane.cpp.
    rowChangedSignal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

static void xpcog_playlist_row_init(XpcogPlaylistRow* self) {
    self->view  = nullptr;
    self->id    = xpcog::kInvalidTrackId;
    self->index = 0;
}

// --- the model --------------------------------------------------------------------

namespace {

struct ModelImpl {
    PlaylistView* view = nullptr;

    /// The rows in display order. Each is owned here; the cache below borrows.
    std::vector<xpcog::gtk::GObjectPtr<XpcogPlaylistRow>> rows;
    /// The same objects by track, so a rebuild hands the same object back for
    /// a track that is still there.
    std::unordered_map<TrackId, XpcogPlaylistRow*> byTrack;

    xpcog::Subscription rebuilt;
    xpcog::Subscription rowChanged;
};

}  // namespace

struct _XpcogPlaylistModel {
    GObject parent_instance;

    ModelImpl* impl;
};

static void xpcog_playlist_model_list_model_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(XpcogPlaylistModel, xpcog_playlist_model, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                                    xpcog_playlist_model_list_model_init))

static GType xpcog_playlist_model_get_item_type(GListModel*) { return XPCOG_TYPE_PLAYLIST_ROW; }

static guint xpcog_playlist_model_get_n_items(GListModel* list) {
    auto* self = XPCOG_PLAYLIST_MODEL(list);
    return static_cast<guint>(self->impl->rows.size());
}

static gpointer xpcog_playlist_model_get_item(GListModel* list, guint position) {
    auto* self = XPCOG_PLAYLIST_MODEL(list);
    if (position >= self->impl->rows.size()) {
        return nullptr;
    }
    // A full reference, which is what the interface hands out.
    return g_object_ref(self->impl->rows[position].get());
}

static void xpcog_playlist_model_list_model_init(GListModelInterface* iface) {
    iface->get_item_type = xpcog_playlist_model_get_item_type;
    iface->get_n_items   = xpcog_playlist_model_get_n_items;
    iface->get_item      = xpcog_playlist_model_get_item;
}

static void xpcog_playlist_model_finalize(GObject* object) {
    auto* self = XPCOG_PLAYLIST_MODEL(object);
    delete self->impl;
    self->impl = nullptr;
    G_OBJECT_CLASS(xpcog_playlist_model_parent_class)->finalize(object);
}

static void xpcog_playlist_model_class_init(XpcogPlaylistModelClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = xpcog_playlist_model_finalize;
}

static void xpcog_playlist_model_init(XpcogPlaylistModel* self) { self->impl = new ModelImpl(); }

namespace {

/// Rebuilds the row vector from the view, reusing the object for every track
/// that is still there, and announces the whole range as changed.
void rebuildRows(XpcogPlaylistModel* self) {
    ModelImpl& impl = *self->impl;
    const auto before = static_cast<guint>(impl.rows.size());

    std::vector<xpcog::gtk::GObjectPtr<XpcogPlaylistRow>> fresh;
    std::unordered_map<TrackId, XpcogPlaylistRow*>        seen;
    const std::size_t                                     count = impl.view->rowCount();
    fresh.reserve(count);
    seen.reserve(count);

    for (std::size_t row = 0; row < count; ++row) {
        const TrackId id    = impl.view->trackAt(row);
        const auto    found = impl.byTrack.find(id);
        xpcog::gtk::GObjectPtr<XpcogPlaylistRow> object;
        if (found != impl.byTrack.end()) {
            object = xpcog::gtk::GObjectPtr<XpcogPlaylistRow>::ref(found->second);
        } else {
            object = xpcog::gtk::GObjectPtr<XpcogPlaylistRow>::adopt(
                XPCOG_PLAYLIST_ROW(g_object_new(XPCOG_TYPE_PLAYLIST_ROW, nullptr)));
            object.get()->view = impl.view;
            object.get()->id   = id;
        }
        object.get()->index = static_cast<guint>(row);
        seen[id]            = object.get();
        fresh.push_back(std::move(object));
    }

    impl.rows    = std::move(fresh);
    impl.byTrack = std::move(seen);
    g_list_model_items_changed(G_LIST_MODEL(self), 0, before, static_cast<guint>(impl.rows.size()));
}

}  // namespace

namespace xpcog::gtk {

XpcogPlaylistModel* playlistModelNew(PlaylistView& view) {
    auto* self = XPCOG_PLAYLIST_MODEL(g_object_new(XPCOG_TYPE_PLAYLIST_MODEL, nullptr));
    self->impl->view = &view;

    self->impl->rebuilt = view.rebuilt.connect([self] { rebuildRows(self); });
    self->impl->rowChanged = view.rowChanged.connect([self](std::size_t row) {
        // Not an items-changed: that would be answered by handing the same
        // object back, which the column view rightly treats as nothing to do.
        // The row tells its cells directly, and the selection is untouched --
        // the case PlaylistView.hpp says must not be a rebuild.
        if (row < self->impl->rows.size()) {
            g_signal_emit(self->impl->rows[row].get(), rowChangedSignal, 0);
        }
    });

    rebuildRows(self);
    return self;
}

TrackId playlistRowTrack(XpcogPlaylistRow* row) { return row->id; }

std::size_t playlistRowIndex(XpcogPlaylistRow* row) { return row->index; }

std::string playlistRowText(XpcogPlaylistRow* row, PlaylistView::Column column) {
    return row->view != nullptr ? row->view->text(row->index, column) : std::string{};
}

const PlaylistEntry* playlistRowEntry(XpcogPlaylistRow* row) {
    return row->view != nullptr ? row->view->entryAt(row->index) : nullptr;
}

bool playlistRowIsCurrent(XpcogPlaylistRow* row) {
    return row->view != nullptr && row->id == row->view->currentTrack();
}

}  // namespace xpcog::gtk
