// The playlist's columns: which there are, how wide they start, how they share
// the width of the control, and how wide they were last time.
//
// The model (PlaylistDataModel) says what is in a cell; this says how the cells
// are laid out. They were one class until the width had to be managed, which
// is a view concern through and through.
//
// **Title takes the slack.** Every toolkit hands the width left over after the
// columns have been laid out to *some* column, and it is never the one a
// listener would pick. wxGTK sets no expand column, so GTK's rule applies: with
// none, the last visible column gets every spare pixel
// (gtk_tree_view_size_allocate_columns, gtktreeview.c). The generic port, which
// is what Windows draws, does the same by hand in
// wxDataViewMainWindow::UpdateColumnSizes (datavgen.cpp). The last column is
// Length, a right-aligned "4:07" -- so a wide window meant a duration column
// wider than the title. Here the leftover goes to Title instead: it is the one
// column whose content is long, and the one people widen the window for.
//
// It is done here, in the application, rather than by asking the toolkit,
// because the toolkit's own answer does not survive the first drag. GTK's
// gtk_tree_view_column_set_expand() is exactly the flag, and GTK's own resize
// handler turns it *off* on any column the listener drags
// (gtk_tree_view_column_drag_gesture_begin -- "set_expand(column, FALSE)"), at
// which point Length is back to swallowing the slack. So Title is not resizable
// by hand on those two ports: its width is whatever the others leave, and a
// listener who wants it narrower widens a neighbour. Every other divider is
// still a drag handle, because the handle belongs to the column on its left.
//
// macOS is left to AppKit. NSTableView autoresizes its columns itself, spreads
// the slack across every resizable one and lets the listener drag any of them,
// and wx exposes no way to say "this one absorbs, the others do not" without
// Objective-C. There, Title is resizable and remembered like the rest.
//
// The widths are remembered between sessions, in dialog units so a DPI change
// does not shift them, under one raw setting key rather than a settings.def
// entry: this is the application's own state, like the window geometry, not a
// Cog preference to stay compatible with.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <wx/dataview.h>
#include <wx/event.h>
#include <wx/timer.h>

namespace xpcog::app {

class PlaylistColumns : public wxEvtHandler {
public:
    /// Appends the columns to `list` -- which must have none yet -- and takes
    /// over their widths from then on. Destroy this before the control.
    PlaylistColumns(wxDataViewCtrl& list, Settings& settings);
    ~PlaylistColumns() override;

    PlaylistColumns(const PlaylistColumns&)            = delete;
    PlaylistColumns& operator=(const PlaylistColumns&) = delete;

    /// Applies the widths saved by persist(). Columns the setting does not
    /// name keep their defaults, and a value that will not parse is ignored.
    void restore();
    /// Records the current widths. Title's is left out where it is derived.
    void persist();

private:
    using Column = PlaylistView::Column;

    /// Gives Title whatever width the other columns leave, and says whether
    /// that changed it. A no-op on macOS.
    bool fit();
    [[nodiscard]] wxDataViewColumn* column(Column column) const;
    /// The width a column was given, by us or by a drag -- as opposed to what
    /// it is currently drawn at, which on two ports includes slack.
    [[nodiscard]] int specifiedWidth(wxDataViewColumn& column) const;
    [[nodiscard]] int availableWidth() const;
    [[nodiscard]] bool pointerButtonDown() const;

#ifdef __WXGTK__
    /// The tree view's "size-allocate". Untyped so that this header need not
    /// name GTK; the .cpp casts.
    static void onGtkSizeAllocate(void* widget, void* allocation, void* self);
#endif

    wxDataViewCtrl& list_;
    Settings&       settings_;
    /// The width last allocated to the tree view, to tell a window resize
    /// from a column drag; see onGtkSizeAllocate().
    int             lastWidth_ = -1;
    /// Polls for the mouse button to come up after a column drag, and fits
    /// then; see onGtkSizeAllocate().
    wxTimer         retry_;
};

}  // namespace xpcog::app
