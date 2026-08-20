// The playlist as a wxDataViewCtrl model.
//
// Thin on purpose. Everything that decides *what* is shown -- the order, the
// filter, the cell text -- is core's PlaylistView; this is the adapter that lets
// wx ask it. Under Qt those two were one class plus a proxy, and pulling them
// apart is what let the sort and filter tests stop needing a display.
//
// `wxDataViewVirtualListModel` rather than a real one because the playlist is
// long and already stored elsewhere: the control asks for the rows it is about
// to draw and nothing is duplicated.
//
// **Why wxDataViewCtrl and not wxListCtrl.** wxListCtrl in virtual mode is the
// more native control on Windows, and drop-*between*-rows there has to be
// hand-drawn. The playlist's drag-reorder is a first-class feature, and
// wxDataViewEvent::GetProposedDropIndex() answers exactly the question the old
// dropMimeData(row, ...) needed. That is a deliberate trade against native feel
// on one platform, and it is recorded as such in docs/WXPORT.md.
//
// One thing that is simply gone rather than ported: PlaylistModel's `rows_`
// member. Qt required the pre-removal row count to still be reported from inside
// beginRemoveRows(), while Playlist notifies *after* it has changed -- so the
// adapter kept a second count and advanced it between the begin and the end
// calls. Getting it wrong aborted on select-all-and-delete. wx's
// RowsDeleted()/Reset() are told after the fact, which is the order Playlist
// already speaks in, so there is no second count to keep in step.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <wx/dataview.h>

#include <vector>

namespace xpcog::app {

class PlaylistDataModel : public wxDataViewVirtualListModel {
public:
    explicit PlaylistDataModel(PlaylistView& view);

    /// The columns, in display order, with their headings and alignment. Built
    /// here rather than by the frame so the model and the control cannot
    /// disagree about how many there are.
    void appendColumnsTo(wxDataViewCtrl* control) const;

    // --- wxDataViewVirtualListModel ---------------------------------------
    //
    // GetColumnCount() and GetColumnType() are deliberately not overridden: wx
    // 3.3 deprecates both, because the control knows its own columns and every
    // value here is a string.
    void GetValueByRow(wxVariant& value, unsigned int row,
                       unsigned int column) const override;
    bool SetValueByRow(const wxVariant& value, unsigned int row,
                       unsigned int column) override;

    /// Greys a row whose file could not be opened, and leans the playing one.
    /// Cog marks a dead entry the same way rather than removing it: the file may
    /// come back, and silently dropping rows from a playlist is worse than
    /// showing one that will not play.
    bool GetAttrByRow(unsigned int row, unsigned int column,
                      wxDataViewItemAttr& attr) const override;

private:
    PlaylistView& view_;

    Subscription rebuiltSubscription_;
    Subscription rowSubscription_;
};

}  // namespace xpcog::app
