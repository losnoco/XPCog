// The playlist as a GListModel.
//
// Thin on purpose, the way app-winui/src/PlaylistTable.hpp is: everything that
// decides *what* is shown -- the order, the filter, the cell text -- is core's
// PlaylistView, and this is the adapter that lets GTK ask it. The two GObject
// types here are the only G_DEFINE_TYPEs in the frontend, and they exist
// because a GtkColumnView takes a GListModel and nothing else.
//
// **Identity is the track, not the row.** A GtkMultiSelection remembers what
// is selected by the item *object*, and re-derives the positions after any
// items-changed. So one XpcogPlaylistRow stands for one TrackId for as long
// as that track is in the playlist, whatever row it moves to: a sort, a
// filter or an insert above it rebuilds the row vector but hands the same
// object back at its new position, and the selection survives without anyone
// saving and restoring it.
//
// **A changed row says so itself.** GtkColumnView keeps a cell bound to the
// same object across an items-changed that hands the object back -- which is
// right for the rebuild above and wrong for a row whose text changed: a
// stream renamed itself, the play marker moved. So the row carries a
// "changed" signal, a bound cell listens to it, and PlaylistView::rowChanged
// becomes one emission. The cells read the text at bind time rather than
// through properties: a property per column would be forty lines of
// boilerplate to say what PlaylistView::text() already says.

#pragma once

#include "xpcog/core/library/PlaylistView.hpp"

#include <gio/gio.h>

G_BEGIN_DECLS

#define XPCOG_TYPE_PLAYLIST_ROW (xpcog_playlist_row_get_type())
G_DECLARE_FINAL_TYPE(XpcogPlaylistRow, xpcog_playlist_row, XPCOG, PLAYLIST_ROW, GObject)

#define XPCOG_TYPE_PLAYLIST_MODEL (xpcog_playlist_model_get_type())
G_DECLARE_FINAL_TYPE(XpcogPlaylistModel, xpcog_playlist_model, XPCOG, PLAYLIST_MODEL, GObject)

G_END_DECLS

namespace xpcog::gtk {

/// A model over `view`. The view outlives the model; the session owns it.
[[nodiscard]] XpcogPlaylistModel* playlistModelNew(PlaylistView& view);

/// The track a row stands for.
[[nodiscard]] TrackId playlistRowTrack(XpcogPlaylistRow* row);

/// The row's position now, which is the index PlaylistView::text() wants.
[[nodiscard]] std::size_t playlistRowIndex(XpcogPlaylistRow* row);

/// One cell's text, from the view.
[[nodiscard]] std::string playlistRowText(XpcogPlaylistRow* row, PlaylistView::Column column);

/// The entry behind the row, or null once it has left the playlist.
[[nodiscard]] const PlaylistEntry* playlistRowEntry(XpcogPlaylistRow* row);

/// Whether the row is the audible track.
[[nodiscard]] bool playlistRowIsCurrent(XpcogPlaylistRow* row);

}  // namespace xpcog::gtk
