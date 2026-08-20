#include "PlaylistDataModel.hpp"

#include "Text.hpp"

namespace xpcog::app {
namespace {

using Column = PlaylistView::Column;

constexpr unsigned int kColumnCount = static_cast<unsigned int>(Column::Count);

/// Widths in dialog units rather than pixels, so they mean the same thing at
/// every DPI. The status column is exactly wide enough for a glyph or a
/// two-digit queue position.
struct ColumnLayout {
    Column      column;
    int         width;
    wxAlignment alignment;
};

constexpr ColumnLayout kColumns[] = {
    {Column::Status, 28, wxALIGN_CENTER},
    {Column::Track, 44, wxALIGN_RIGHT},
    {Column::Title, 280, wxALIGN_LEFT},
    {Column::Artist, 180, wxALIGN_LEFT},
    {Column::Album, 180, wxALIGN_LEFT},
    {Column::Length, 64, wxALIGN_RIGHT},
};

}  // namespace

PlaylistDataModel::PlaylistDataModel(PlaylistView& view) : view_(view) {
    Reset(static_cast<unsigned int>(view_.rowCount()));

    rebuiltSubscription_ = view_.rebuilt.connect([this] {
        // Reset rather than a row delta: with a sort and a filter in the way an
        // insertion can land anywhere or nowhere, and the control is virtual, so
        // this costs a redraw of what is on screen. What it does cost is the
        // selection, which is why the in-place case below is separate.
        Reset(static_cast<unsigned int>(view_.rowCount()));
    });

    rowSubscription_ = view_.rowChanged.connect([this](std::size_t row) {
        RowValueChanged(static_cast<unsigned int>(row), 0);
        // Every column: a tag read finishing during a scan fills in title,
        // artist, album and length at once, and reporting only one leaves the
        // rest showing the filename.
        for (unsigned int column = 0; column < kColumnCount; ++column) {
            RowValueChanged(static_cast<unsigned int>(row), column);
        }
    });
}

void PlaylistDataModel::appendColumnsTo(wxDataViewCtrl* control) const {
    for (const ColumnLayout& layout : kColumns) {
        const wxString heading = toWx(PlaylistView::heading(layout.column));
        // Sortable for the header affordance only -- the arrow and the click
        // target. The sorting itself is PlaylistView's, and the frame drives it
        // from the header-click event; letting the control sort a virtual model
        // is not something every platform's native implementation supports.
        control->AppendTextColumn(heading, static_cast<unsigned int>(layout.column),
                                  wxDATAVIEW_CELL_INERT,
                                  control->FromDIP(layout.width), layout.alignment,
                                  wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    }
}

void PlaylistDataModel::GetValueByRow(wxVariant& value, unsigned int row,
                                      unsigned int column) const {
    if (column >= kColumnCount) {
        value = wxString{};
        return;
    }
    value = toWx(view_.text(row, static_cast<Column>(column)));
}

bool PlaylistDataModel::SetValueByRow(const wxVariant& value, unsigned int row,
                                      unsigned int column) {
    // Nothing here is editable in place. Tags are changed by re-reading the
    // file, not by typing into the list, which is Cog's behaviour too.
    (void)value;
    (void)row;
    (void)column;
    return false;
}

bool PlaylistDataModel::GetAttrByRow(unsigned int row, unsigned int column,
                                     wxDataViewItemAttr& attr) const {
    (void)column;

    const PlaylistEntry* entry = view_.entryAt(row);
    if (entry == nullptr) {
        return false;
    }

    bool decorated = false;
    if (entry->error) {
        // Greyed rather than removed: the file may come back, and silently
        // dropping rows from a playlist is worse than showing one that will not
        // play. The warning glyph in the status column says why.
        attr.SetColour(*wxLIGHT_GREY);
        decorated = true;
    }
    if (entry->id == view_.currentTrack()) {
        attr.SetBold(true);
        decorated = true;
    }
    return decorated;
}

}  // namespace xpcog::app
