#include "PlaylistDataModel.hpp"

#include "Text.hpp"

#include <wx/translation.h>

#include <string_view>

namespace xpcog::app {
namespace {

using Column = PlaylistView::Column;

constexpr unsigned int kColumnCount = static_cast<unsigned int>(Column::Count);

/// Widths in dialog units rather than pixels, so they mean the same thing at
/// every DPI. The status column is exactly wide enough for a glyph or a
/// two-digit queue position.
///
/// **The status column is deliberately not resizable**, and that is what keeps
/// it visible on macOS rather than a matter of taste. `wxDATAVIEW_COL_RESIZABLE`
/// gives the NSTableColumn `NSTableColumnAutoresizingMask`, and AppKit then
/// redistributes the widths across the columns whenever the control is resized
/// -- which first happens while the control is still at its default size, before
/// the splitter has given it any. Every column is squeezed proportionally, a
/// minimum width of zero lets the narrowest one go all the way, and the growth
/// afterwards is proportional too, so a column that reached 2 points stays at 2
/// points for the rest of the session. Measured, not guessed: the column came
/// back 2 wide with a cell rectangle of 2x17, against a glyph 11 wide.
///
/// A glyph column has no business being resized or autoresized anyway, and
/// `NSTableColumnNoResizing` takes it out of that arithmetic entirely. The
/// minimum width set alongside it is the same statement made a second way, for
/// the ports that have no such flag.
struct ColumnLayout {
    Column      column;
    int         width;
    wxAlignment alignment;
    bool        resizable;
};

constexpr ColumnLayout kColumns[] = {
    {Column::Status, 28, wxALIGN_CENTER, false},
    {Column::Track, 44, wxALIGN_RIGHT, true},
    {Column::Title, 280, wxALIGN_LEFT, true},
    {Column::Artist, 180, wxALIGN_LEFT, true},
    {Column::Album, 180, wxALIGN_LEFT, true},
    {Column::Length, 64, wxALIGN_RIGHT, true},
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
        // Translated here, which is what PlaylistView::heading()'s "a front end
        // that wants them localised should map them" means in practice: core
        // links no toolkit and so has no catalogue to consult. The msgids are
        // core's own spellings -- "#", "Title", "Artist", "Album", "Length" --
        // and app/locale/xpcog.pot carries them with a comment saying where
        // they come from, because nothing in this directory declares them.
        //
        // The empty one is not looked up, and that is not defensiveness: msgid
        // "" is the catalogue's *header*, so asking for it returns the charset
        // and plural-rule block rather than nothing. The status column's
        // heading is deliberately blank -- it is a glyph column -- and would
        // otherwise draw several lines of metadata.
        const std::string_view english = PlaylistView::heading(layout.column);
        const wxString         heading =
            english.empty() ? wxString{} : wxGetTranslation(toWx(english));
        // Sortable for the header affordance -- the arrow and the click target.
        // The sorting itself is PlaylistView's, and the frame drives it from the
        // header-click event. The flag does not merely decorate, though: it also
        // licenses the control to reorder the rows on its own, which is what
        // Compare() below is there to make a no-op.
        const int width = control->FromDIP(layout.width);
        wxDataViewColumn* added = control->AppendTextColumn(
            heading, static_cast<unsigned int>(layout.column), wxDATAVIEW_CELL_INERT,
            width, layout.alignment,
            (layout.resizable ? wxDATAVIEW_COL_RESIZABLE : 0) |
                wxDATAVIEW_COL_SORTABLE);
        if (added != nullptr && !layout.resizable) {
            // A floor as well as a fixed width. Only for the fixed column: on a
            // resizable one this would be a promise the listener cannot narrow
            // past, which is not what a minimum is for here.
            added->SetMinWidth(width);
        }
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

int PlaylistDataModel::Compare(const wxDataViewItem& item1, const wxDataViewItem& item2,
                               unsigned int column, bool ascending) const {
    // A sortable column does not only draw an arrow: the control sorts the rows
    // itself when the header is clicked, and it asks the model how. The default
    // answer, wxDataViewModel::Compare, fetches both cells and compares the
    // *formatted text* with wxString::Cmp -- so the track column came out 1, 10,
    // 100, 1000, 2, and the length column put 10:00 before 4:07. That sort ran on
    // top of the numeric one PlaylistView had already done, and won, because it
    // ran last.
    //
    // It is not the comparison that is wrong here, it is doing one at all. Row
    // order is the answer: PlaylistView has already ordered the mapping -- digit
    // runs as numbers for text columns, the stored value for numeric ones -- and
    // sorting by row index is the no-op that leaves that order standing. Every
    // platform's control resorts on Reset() too, so this has to hold for the
    // reload after a scan, not just for the click.
    //
    // `column` and `ascending` are both deliberately ignored. Descending is
    // already in the mapping, because the header click told the view about it;
    // honouring the flag here would reverse it a second time and hand back the
    // ascending order. That is what the non-Mac wxDataViewVirtualListModel::Compare
    // does, which is why this override is not a macOS special case.
    (void)column;
    (void)ascending;

    // Ids are row+1 and stay that way: the model only ever calls Reset(), never
    // RowDeleted/RowInserted, so GetRow() is arithmetic rather than a search.
    const unsigned int left  = GetRow(item1);
    const unsigned int right = GetRow(item2);
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
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
