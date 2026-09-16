#include "PlaylistColumns.hpp"

#include "Text.hpp"

#include <wx/headerctrl.h>
#include <wx/translation.h>
#include <wx/utils.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

namespace xpcog::app {
namespace {

using Column = PlaylistView::Column;

/// The one raw setting the widths live under. See the header for why raw.
constexpr std::string_view kWidthsKey = "xpcog.playlist.columns";

/// Whether the toolkit distributes the control's width over the columns by
/// itself. True on macOS, where AppKit does and cannot be told not to; see
/// the header. Where it is false, fit() is the distribution.
#ifdef __WXOSX__
constexpr bool kToolkitFills = true;
#else
constexpr bool kToolkitFills = false;
#endif

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
///
/// `key` is the column's name in the saved widths. Title's width is the
/// default for a session with nothing saved and for macOS; elsewhere it is
/// overwritten by fit() before the control is first drawn.
struct ColumnLayout {
    Column           column;
    std::string_view key;
    int              width;
    wxAlignment      alignment;
    bool             fixed;  ///< never resized, by anyone
    bool             fills;  ///< takes the slack, where fit() is in charge
};

constexpr ColumnLayout kColumns[] = {
    {Column::Status, "status", 28, wxALIGN_CENTER, true, false},
    {Column::Track, "track", 44, wxALIGN_RIGHT, false, false},
    {Column::Title, "title", 280, wxALIGN_LEFT, false, true},
    {Column::Artist, "artist", 180, wxALIGN_LEFT, false, false},
    {Column::Album, "album", 180, wxALIGN_LEFT, false, false},
    {Column::Length, "length", 64, wxALIGN_RIGHT, false, false},
};

/// Narrower than this and the title is not a title. A floor for fit(), and on
/// GTK also the floor GTK itself allocates to, so the two cannot disagree.
constexpr int kMinFillWidth = 80;

/// Whether the listener may drag this column's divider.
constexpr bool userResizable(const ColumnLayout& layout) {
    return !layout.fixed && (!layout.fills || kToolkitFills);
}

/// Whether the column's width is worth remembering: one the listener can set.
constexpr bool remembered(const ColumnLayout& layout) {
    return userResizable(layout);
}

const ColumnLayout* layoutFor(std::string_view key) {
    for (const ColumnLayout& layout : kColumns) {
        if (layout.key == key) {
            return &layout;
        }
    }
    return nullptr;
}

}  // namespace

PlaylistColumns::PlaylistColumns(wxDataViewCtrl& list, Settings& settings)
    : list_(list), settings_(settings), retry_(this) {
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
        // PlaylistDataModel::Compare() is there to make a no-op.
        const int width = list_.FromDIP(layout.width);
        wxDataViewColumn* added = list_.AppendTextColumn(
            heading, static_cast<unsigned int>(layout.column), wxDATAVIEW_CELL_INERT,
            width, layout.alignment,
            (userResizable(layout) ? wxDATAVIEW_COL_RESIZABLE : 0) |
                wxDATAVIEW_COL_SORTABLE);
        if (added == nullptr) {
            continue;
        }
        // A floor as well as a width, but only where nobody drags the column:
        // on one the listener resizes this would be a promise they cannot
        // narrow past, which is not what a minimum is for here. The status
        // column's floor is its width; the filler's is kMinFillWidth.
        if (layout.fixed) {
            added->SetMinWidth(width);
        } else if (!userResizable(layout)) {
            added->SetMinWidth(list_.FromDIP(kMinFillWidth));
        }
    }

#if defined(__WXGTK__)
    // The tree view's own allocation, not the wxDataViewCtrl's size event.
    // The control is a GtkScrolledWindow and wx reports its size, which is
    // not the tree view's whenever a non-overlay vertical scrollbar has
    // appeared -- and that happens as rows are added, with no size event at
    // all. The tree view is re-allocated on every column drag too, which is
    // how a drag's aftermath is caught; see onGtkSizeAllocate().
    g_signal_connect(list_.GtkGetTreeView(), "size-allocate",
                     G_CALLBACK(&PlaylistColumns::onGtkSizeAllocate), this);
#elif defined(wxHAS_GENERIC_DATAVIEWCTRL)
    // Bound rather than in a subclass so this handler runs *before* the
    // control's own wxDataViewCtrl::OnSize, whose UpdateColumnSizes() widens
    // the last column into whatever is left. With Title already fitted there
    // is nothing left, and Length keeps the width it was given.
    list_.Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        fit();
    });
    // The end of a drag, once: fitting while the drag is still going would
    // move Title, and with it the divider the listener is holding. CallAfter
    // because the header's own handler, which applies the new width, runs
    // after this one.
    if (wxHeaderCtrl* header = list_.GenericGetHeader(); header != nullptr) {
        header->Bind(wxEVT_HEADER_END_RESIZE, [this](wxHeaderCtrlEvent& event) {
            event.Skip();
            CallAfter([this] { fit(); });
        });
    }
#endif

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!pointerButtonDown()) {
            retry_.Stop();
            fit();
        }
    });
}

PlaylistColumns::~PlaylistColumns() {
    retry_.Stop();
#ifdef __WXGTK__
    g_signal_handlers_disconnect_by_data(list_.GtkGetTreeView(), this);
#endif
}

#ifdef __WXGTK__
void PlaylistColumns::onGtkSizeAllocate(void* widget, void* allocation, void* self) {
    auto* columns = static_cast<PlaylistColumns*>(self);
    auto* tree    = static_cast<GtkWidget*>(widget);
    auto* rect    = static_cast<GtkAllocation*>(allocation);

    // A change of width is the window or the scrollbar and is fitted at once;
    // that the mouse button is down during a window resize on X11 is of no
    // account. The same width again is a column drag in progress -- GTK
    // re-allocates the tree on every motion of one -- and that waits for the
    // button to come up, since moving Title under the drag moves the divider
    // being dragged.
    bool fitted = false;
    if (rect->width != columns->lastWidth_) {
        columns->lastWidth_ = rect->width;
        fitted              = columns->fit();
    } else if (columns->pointerButtonDown()) {
        if (!columns->retry_.IsRunning()) {
            columns->retry_.Start(50);
        }
    } else {
        fitted = columns->fit();
    }
    if (!fitted) {
        return;
    }

    // The columns have already been laid out by the time this handler runs --
    // the class handler goes first -- and with the old Title width. Setting
    // the new one queued a resize, but GTK discards a resize queued from
    // inside an allocation ("Size allocation is god... no further requests or
    // allocations are needed", gtk_widget_size_allocate_with_baseline), so
    // nothing would lay the columns out again until the window next changed
    // size; the tree would sit there with Length wide. Running the class
    // handler a second time lays them out now, into the same allocation. It
    // is written to be run repeatedly: it stores the allocation, sizes the
    // columns, syncs the adjustments and moves its windows, and does not emit
    // this signal again.
    GTK_WIDGET_GET_CLASS(tree)->size_allocate(tree, rect);
}
#endif

bool PlaylistColumns::pointerButtonDown() const {
    return wxGetMouseState().LeftIsDown();
}

wxDataViewColumn* PlaylistColumns::column(Column column) const {
    // By model column rather than by position: the two part company the moment
    // a column is dragged to a new place.
    for (unsigned int i = 0; i < list_.GetColumnCount(); ++i) {
        wxDataViewColumn* candidate = list_.GetColumn(i);
        if (candidate != nullptr &&
            static_cast<Column>(candidate->GetModelColumn()) == column) {
            return candidate;
        }
    }
    return nullptr;
}

int PlaylistColumns::specifiedWidth(wxDataViewColumn& column) const {
#if defined(__WXGTK__)
    // GetWidth() is gtk_tree_view_column_get_width(), the *allocated* width,
    // and for the last column that is its fixed width plus the slack. Reading
    // that back as the width to fit around would hand the slack to Length
    // again on every pass. The fixed width is what SetWidth() and a drag set.
    auto* native = GTK_TREE_VIEW_COLUMN(column.GetGtkHandle());
    if (const int fixed = gtk_tree_view_column_get_fixed_width(native); fixed >= 0) {
        return fixed;
    }
    return column.GetWidth();
#elif defined(wxHAS_GENERIC_DATAVIEWCTRL)
    // The width last set explicitly, by SetWidth() or a drag, as opposed to
    // m_width, which UpdateColumnSizes() bumps on the last column.
    return column.WXGetSpecifiedWidth();
#else
    return column.GetWidth();
#endif
}

int PlaylistColumns::availableWidth() const {
#if defined(__WXGTK__)
    // The same number gtk_tree_view_size_allocate_columns() lays the columns
    // out into.
    GtkAllocation allocation{};
    gtk_widget_get_allocation(list_.GtkGetTreeView(), &allocation);
    return allocation.width;
#else
    return list_.GetClientSize().x;
#endif
}

bool PlaylistColumns::fit() {
    if (kToolkitFills) {
        return false;
    }
    wxDataViewColumn* title = column(Column::Title);
    if (title == nullptr) {
        return false;
    }
    int available = availableWidth();
    for (unsigned int i = 0; i < list_.GetColumnCount(); ++i) {
        wxDataViewColumn* other = list_.GetColumn(i);
        if (other != nullptr && other != title && !other->IsHidden()) {
            available -= specifiedWidth(*other);
        }
    }
    // Below the floor the columns no longer fit and the control scrolls
    // sideways, which is the right answer to a window narrower than its
    // columns; Title shrinking to nothing would not be.
    const int width = std::max(available, title->GetMinWidth());
    if (width == specifiedWidth(*title)) {
        return false;
    }
    title->SetWidth(width);
    return true;
}

void PlaylistColumns::restore() {
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
        const ColumnLayout* layout = layoutFor(entry.substr(0, equals));
        if (layout == nullptr || !remembered(*layout)) {
            // A column that is no longer resizable, or that this version does
            // not know: the default stands.
            continue;
        }
        const std::string_view digits = entry.substr(equals + 1);
        int                    dips   = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), dips);
        // Bounded: a hand-edited value cannot make a column vanish, or push
        // the rest off the screen.
        if (error != std::errc{} || end != digits.data() + digits.size() ||
            dips < 8 || dips > 4000) {
            continue;
        }
        if (wxDataViewColumn* target = column(layout->column); target != nullptr) {
            target->SetWidth(list_.FromDIP(dips));
        }
    }
    fit();
}

void PlaylistColumns::persist() {
    std::string value;
    for (const ColumnLayout& layout : kColumns) {
        if (!remembered(layout)) {
            continue;
        }
        wxDataViewColumn* target = column(layout.column);
        if (target == nullptr) {
            continue;
        }
        if (!value.empty()) {
            value += ',';
        }
        value += layout.key;
        value += '=';
        value += std::to_string(list_.ToDIP(specifiedWidth(*target)));
    }
    settings_.setRawValue(kWidthsKey, value);
}

}  // namespace xpcog::app
