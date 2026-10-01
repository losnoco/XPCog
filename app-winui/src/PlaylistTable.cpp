// The playlist as a WinUI ListView.
//
// WinUI has no multi-column table for C++ -- the Community Toolkit DataGrid is
// C# only -- so this is a ListView whose rows are a Grid of TextBlocks, under a
// header of our own: a Grid with the same column definitions, whose cells sort
// when clicked, reorder when dragged, and resize from a gripper on their right
// edge. The row template and the item style are XAML text loaded with
// XamlReader, which needs no XAML compiler.
//
// Rows are filled in ContainerContentChanging rather than bound. That is the
// fast path the ListView documentation recommends for large lists anyway, and
// here it is also the only reasonable one: {x:Bind} needs the compiler, and
// {Binding} on a C++/WinRT object needs ICustomPropertyProvider for every row.
//
// The items themselves are a virtual collection (RowSource): a count, and a row
// number boxed on demand for whichever rows WinUI asks about. Nothing is held
// for rows that are not on screen, so a playlist of a million tracks costs the
// list what a playlist of ten does.

#include "PlaylistTable.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include "Translations.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <cmath>
#include <string>
#include <vector>

namespace xpcog::winui {

namespace {

using Column       = PlaylistView::Column;
using IInspectable = winrt::Windows::Foundation::IInspectable;
namespace wfc      = winrt::Windows::Foundation::Collections;

// --- the items ------------------------------------------------------------------

struct ResetArgs : winrt::implements<ResetArgs, wfc::IVectorChangedEventArgs> {
    wfc::CollectionChange CollectionChange() const { return wfc::CollectionChange::Reset; }
    uint32_t              Index() const { return 0; }
};

struct RowIterator : winrt::implements<RowIterator, wfc::IIterator<IInspectable>> {
    explicit RowIterator(uint32_t count) : count_(count) {}

    IInspectable Current() const {
        if (at_ >= count_) {
            throw winrt::hresult_out_of_bounds();
        }
        return winrt::box_value(static_cast<int32_t>(at_));
    }
    bool HasCurrent() const { return at_ < count_; }
    bool MoveNext() {
        if (at_ < count_) {
            ++at_;
        }
        return at_ < count_;
    }
    uint32_t GetMany(winrt::array_view<IInspectable> items) {
        uint32_t n = 0;
        for (; n < items.size() && at_ < count_; ++n, ++at_) {
            items[n] = winrt::box_value(static_cast<int32_t>(at_));
        }
        return n;
    }

private:
    uint32_t count_;
    uint32_t at_ = 0;
};

/// Row numbers, 0 to count - 1, made when asked for. Read-only: the ListView
/// never edits its source unless told it may reorder, and it is not told.
///
/// Equality is by value rather than identity -- IndexOf unboxes -- because a
/// row's item is a fresh box every time GetAt is asked for it. That is what
/// selection, ScrollIntoView and container recycling all look rows up through.
struct RowSource : winrt::implements<RowSource, wfc::IObservableVector<IInspectable>,
                                     wfc::IVector<IInspectable>, wfc::IVectorView<IInspectable>,
                                     wfc::IIterable<IInspectable>> {
    void reset(uint32_t count) {
        count_ = count;
        changed_(*this, winrt::make<ResetArgs>());
    }

    IInspectable GetAt(uint32_t index) const {
        if (index >= count_) {
            throw winrt::hresult_out_of_bounds();
        }
        return winrt::box_value(static_cast<int32_t>(index));
    }
    uint32_t Size() const { return count_; }
    bool     IndexOf(IInspectable const& value, uint32_t& index) const {
        const auto boxed = value.try_as<winrt::Windows::Foundation::IReference<int32_t>>();
        if (!boxed || boxed.Value() < 0 || static_cast<uint32_t>(boxed.Value()) >= count_) {
            return false;
        }
        index = static_cast<uint32_t>(boxed.Value());
        return true;
    }
    uint32_t GetMany(uint32_t start, winrt::array_view<IInspectable> items) const {
        uint32_t n = 0;
        for (; n < items.size() && start + n < count_; ++n) {
            items[n] = winrt::box_value(static_cast<int32_t>(start + n));
        }
        return n;
    }
    wfc::IVectorView<IInspectable> GetView() { return *this; }
    wfc::IIterator<IInspectable>   First() const { return winrt::make<RowIterator>(count_); }

    void SetAt(uint32_t, IInspectable const&) { readOnly(); }
    void InsertAt(uint32_t, IInspectable const&) { readOnly(); }
    void RemoveAt(uint32_t) { readOnly(); }
    void Append(IInspectable const&) { readOnly(); }
    void RemoveAtEnd() { readOnly(); }
    void Clear() { readOnly(); }
    void ReplaceAll(winrt::array_view<IInspectable const>) { readOnly(); }

    winrt::event_token VectorChanged(wfc::VectorChangedEventHandler<IInspectable> const& handler) {
        return changed_.add(handler);
    }
    void VectorChanged(winrt::event_token const& token) noexcept { changed_.remove(token); }

private:
    [[noreturn]] static void readOnly() { throw winrt::hresult_illegal_method_call(); }

    uint32_t                                                   count_ = 0;
    winrt::event<wfc::VectorChangedEventHandler<IInspectable>> changed_;
};

// --- the columns ----------------------------------------------------------------

/// The wx list's table, PlaylistColumns.cpp's kColumns, down to the keys: the
/// two lists read and write the same saved widths, so switching between them
/// keeps the layout. Title takes the slack and the status glyph never moves.
struct ColumnSpec {
    Column           column;
    std::string_view key;
    double           width;  ///< DIPs, before anything is saved
    bool             right;
    bool             fixed;
    bool             fills;
};

constexpr std::array kColumns{
    ColumnSpec{Column::Status, "status", 28, false, true, false},
    ColumnSpec{Column::Track, "track", 44, true, false, false},
    ColumnSpec{Column::Title, "title", 280, false, false, true},
    ColumnSpec{Column::Artist, "artist", 180, false, false, false},
    ColumnSpec{Column::Album, "album", 180, false, false, false},
    ColumnSpec{Column::Length, "length", 64, true, false, false},
};

constexpr std::string_view kWidthsKey = "xpcog.playlist.columns";
constexpr double           kMinWidth  = 24;
constexpr double           kSpacing   = 12;
/// The header's filter button, and the empty column under it in the rows.
constexpr double kToolWidth = 32;
/// Pointer travel before a press on a heading is a drag rather than a click.
constexpr double kDragThreshold = 6;

// Denser than the default 40px row, which is a touch target: a playlist is read
// in bulk and wants more rows on screen. BasedOn keeps everything else of the
// stock item, the selection pill and the hover included.
constexpr const wchar_t* kItemStyle =
    L"<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
    L" TargetType='ListViewItem' BasedOn='{StaticResource DefaultListViewItemStyle}'>"
    L"<Setter Property='MinHeight' Value='28'/>"
    L"<Setter Property='Padding' Value='12,0,12,0'/>"
    L"<Setter Property='HorizontalContentAlignment' Value='Stretch'/>"
    L"</Style>";

// The row: one TextBlock per column. Which column each one shows, and how wide
// its cell is, is applied in code, because both change when the header does.
std::wstring rowTemplate() {
    std::wstring xaml =
        L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>"
        L"<Grid ColumnSpacing='12'>";
    for (std::size_t i = 0; i < kColumns.size(); ++i) {
        xaml += L"<TextBlock Grid.Column='" + std::to_wstring(i) +
                L"' VerticalAlignment='Center' TextTrimming='CharacterEllipsis'/>";
    }
    return xaml + L"</Grid></DataTemplate>";
}

template <typename T>
T load(const std::wstring& xaml) {
    return mux::Markup::XamlReader::Load(xaml).as<T>();
}

/// The resize handle on a heading's right edge. A class of its own only to
/// set the cursor: ProtectedCursor is protected, so nothing outside a subclass
/// can give an element the east-west arrows.
struct Gripper : mux::Controls::GridT<Gripper> {
    Gripper() {
        ProtectedCursor(winrt::Microsoft::UI::Input::InputSystemCursor::Create(
            winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast));
    }
};

}  // namespace

// --- the table ------------------------------------------------------------------

struct PlaylistTable::Impl {
    Impl(PlaylistTable& o, PlaylistView& v, Settings& s) : owner(o), view(v), settings(s) {
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            order[i]  = i;
            widths[i] = kColumns[i].width;
        }
        restoreWidths();
    }

    PlaylistTable& owner;
    PlaylistView&  view;
    Settings&      settings;

    mux::Controls::ListView list{nullptr};
    mux::Controls::Grid     header{nullptr};
    mux::Controls::Border   headerFrame{nullptr};
    mux::Controls::Button   filterButton{nullptr};
    mux::Controls::Grid     filterBar{nullptr};
    mux::Controls::TextBox  filterBox{nullptr};
    mux::Controls::Grid     root{nullptr};
    winrt::com_ptr<RowSource> rows;

    /// Display position -> index into kColumns. Session-only: the wx list has no
    /// column order to share, and a new setting is not a prototype's to add.
    std::array<std::size_t, kColumns.size()> order{};
    std::array<double, kColumns.size()>      widths{};
    /// Bumped whenever order or widths change; a row's Tag says which layout it
    /// was last given, so scrolling does not rebuild column definitions.
    int32_t layout = 0;
    /// The share of their saved width the wide columns are drawn at; see
    /// shownWidth().
    double fit = 1.0;

    struct Drag {
        bool     pressed  = false;
        bool     dragging = false;
        bool     resizing = false;
        std::size_t position = 0;
        double   startX   = 0;
        double   startWidth = 0;
        /// For a resize: the column whose width the divider changes, and
        /// whether it grows (+1) or shrinks (-1) as the pointer moves right.
        std::size_t target = 0;
        double      sense  = 1;
        /// How far the target may grow before the filling column is at its
        /// floor.
        double room = 0;
    } drag;

    /// What dragging the divider on a heading's right edge resizes, if
    /// anything. The filling column is pinned to neither side, so the columns
    /// to its right are pinned to the table's right edge: widening one of
    /// those from its right edge would move its *left* edge, and the divider
    /// under the pointer would stay put while the one beside it moved. So the
    /// divider moves the column on whichever side of it is away from the
    /// filling column, and the filling column takes up the difference either
    /// way -- the edge goes where the pointer goes.
    struct ResizeTarget {
        std::size_t column;  ///< index into kColumns
        double      sense;
    };
    [[nodiscard]] std::optional<ResizeTarget> resizeTarget(std::size_t position) const {
        std::size_t fill = kColumns.size();
        for (std::size_t p = 0; p < kColumns.size(); ++p) {
            if (kColumns[order[p]].fills) {
                fill = p;
            }
        }
        ResizeTarget target{};
        if (position < fill) {
            target = {order[position], 1};
        } else if (position + 1 < kColumns.size()) {
            target = {order[position + 1], -1};
        } else {
            return std::nullopt;
        }
        const ColumnSpec& spec = kColumns[target.column];
        if (spec.fixed || spec.fills) {
            return std::nullopt;
        }
        return target;
    }

    /// The filling column's floor, PlaylistColumns' kMinFillWidth.
    static constexpr double kMinFillWidth = 80;

    Subscription rebuilt;
    Subscription rowChanged;

    // --- layout ---------------------------------------------------------------

    void restoreWidths() {
        const std::string saved = settings.rawValue(kWidthsKey);
        std::string_view  rest  = saved;
        while (!rest.empty()) {
            const std::size_t      comma = rest.find(',');
            const std::string_view entry = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const std::size_t equals = entry.find('=');
            if (equals == std::string_view::npos) {
                continue;
            }
            for (std::size_t i = 0; i < kColumns.size(); ++i) {
                if (kColumns[i].key != entry.substr(0, equals) || kColumns[i].fixed ||
                    kColumns[i].fills) {
                    continue;
                }
                const std::string_view digits = entry.substr(equals + 1);
                int                    dips   = 0;
                const auto [end, error] =
                    std::from_chars(digits.data(), digits.data() + digits.size(), dips);
                // The same bounds as PlaylistColumns::restore(), for the same
                // reason: a hand-edited value cannot make a column vanish.
                if (error == std::errc{} && end == digits.data() + digits.size() &&
                    dips >= 8 && dips <= 4000) {
                    widths[i] = dips;
                }
            }
        }
    }

    void persistWidths() const {
        std::string value;
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            if (kColumns[i].fixed || kColumns[i].fills) {
                continue;
            }
            if (!value.empty()) {
                value += ',';
            }
            value += kColumns[i].key;
            value += '=';
            value += std::to_string(static_cast<int>(std::lround(widths[i])));
        }
        settings.setRawValue(kWidthsKey, value);
    }

    void applyColumns(const mux::Controls::Grid& grid) const {
        auto definitions = grid.ColumnDefinitions();
        while (definitions.Size() < kColumns.size()) {
            definitions.Append(mux::Controls::ColumnDefinition());
        }
        for (std::size_t p = 0; p < kColumns.size(); ++p) {
            const std::size_t c = order[p];
            definitions.GetAt(static_cast<uint32_t>(p))
                .Width(kColumns[c].fills
                           ? mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)
                           : mux::GridLengthHelper::FromPixels(shownWidth(c)));
        }
        // And one more, the width of the header's filter button, in the rows
        // as well as the header: an empty column under it keeps every column
        // of the rows under its heading.
        if (definitions.Size() < kColumns.size() + 1) {
            definitions.Append(mux::Controls::ColumnDefinition());
        }
        definitions.GetAt(static_cast<uint32_t>(kColumns.size()))
            .Width(mux::GridLengthHelper::FromPixels(kToolWidth));
    }

    // --- the filter -------------------------------------------------------------
    //
    // A button at the header's end, beside what it filters, and a field it
    // opens across the top of the table. Closing the field clears the filter:
    // a filter that is on but out of sight is a playlist that looks as though
    // tracks have gone missing.

    void buildFilter() {
        filterBox = mux::Controls::TextBox();
        filterBox.PlaceholderText(toH(app::tr("Filter")));
        filterBox.TextChanged([this](auto&&, auto&&) {
            view.setFilter(toUtf8(filterBox.Text()));
            showFilterActive();
        });
        filterBox.KeyDown([this](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Escape) {
                closeFilter();
                args.Handled(true);
            }
        });
        mux::Automation::AutomationProperties::SetName(filterBox, toH(app::tr("Filter the playlist")));

        auto close = toolButton(L"\xE711", app::tr("Close"));  // Segoe Fluent "Cancel"
        close.Click([this](auto&&, auto&&) { closeFilter(); });

        filterBar = mux::Controls::Grid();
        filterBar.ColumnSpacing(4);
        filterBar.Padding(mux::ThicknessHelper::FromLengths(12, 8, 8, 4));
        for (const auto& width : {mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                                  mux::GridLengthHelper::Auto()}) {
            auto column = mux::Controls::ColumnDefinition();
            column.Width(width);
            filterBar.ColumnDefinitions().Append(column);
        }
        mux::Controls::Grid::SetColumn(close, 1);
        filterBar.Children().Append(filterBox);
        filterBar.Children().Append(close);
        filterBar.Visibility(mux::Visibility::Collapsed);
    }

    [[nodiscard]] static mux::Controls::Button toolButton(const wchar_t* glyph, const std::string& name) {
        auto icon = mux::Controls::FontIcon();
        icon.Glyph(glyph);
        icon.FontSize(14);
        auto button = mux::Controls::Button();
        button.Content(icon);
        button.Width(kToolWidth);
        button.Height(28);
        button.Padding(mux::ThicknessHelper::FromUniformLength(0));
        button.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
        button.BorderBrush(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
        mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(toH(name)));
        mux::Automation::AutomationProperties::SetName(button, toH(name));
        return button;
    }

    void openFilter() {
        filterBar.Visibility(mux::Visibility::Visible);
        filterBox.Focus(mux::FocusState::Programmatic);
    }

    void closeFilter() {
        filterBox.Text({});  // clears the view's filter through TextChanged
        filterBar.Visibility(mux::Visibility::Collapsed);
        list.Focus(mux::FocusState::Programmatic);
    }

    /// The button in the accent while a filter is applied, so a filtered
    /// playlist says so even with the field scrolled or closed.
    void showFilterActive() const {
        auto glyph = filterButton.Content().as<mux::Controls::FontIcon>();
        if (filterBox.Text().empty()) {
            glyph.ClearValue(mux::Controls::IconElement::ForegroundProperty());
        } else {
            glyph.Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
        }
    }

    /// How wide column `c` is drawn. Its saved width, unless the table is too
    /// narrow for the saved widths and the filling column's floor together --
    /// the side panes take a third of the window, and widths saved in a wider
    /// one would otherwise squeeze Title to nothing and push Length off the
    /// edge. Then the wide columns give way in proportion, and only on screen:
    /// what is saved is still what the listener set.
    [[nodiscard]] double shownWidth(std::size_t c) const {
        return yields(c) ? widths[c] * fit : widths[c];
    }

    /// The columns that give way: the wide ones. The status glyph, the track
    /// number and the length are already as narrow as they can usefully be.
    [[nodiscard]] bool yields(std::size_t c) const {
        return !kColumns[c].fixed && !kColumns[c].fills && widths[c] > 120;
    }

    /// Recomputes `fit` for the table's current width; true when it changed.
    bool refit() {
        const double total = root ? root.ActualWidth() : 0.0;
        double       firm = 0.0, give = 0.0;
        for (std::size_t c = 0; c < kColumns.size(); ++c) {
            if (kColumns[c].fills) {
                continue;
            }
            (yields(c) ? give : firm) += widths[c];
        }
        // The header's padding, the gaps between columns and the scroll bar.
        // ... and the filter button's column and the gap before it.
        const double chrome =
            32 + kSpacing * static_cast<double>(kColumns.size()) + 16 + kToolWidth;
        const double room   = total - chrome - firm - kMinFillWidth;
        const double next   = (total <= 0 || give <= 0 || room >= give) ? 1.0 : std::max(0.25, room / give);
        if (std::abs(next - fit) < 0.005) {
            return false;
        }
        fit = next;
        return true;
    }

    void relayout() {
        ++layout;
        applyColumns(header);
        refreshHeader();
        // Only the realized rows: the rest are laid out as they are recycled
        // into view, by fill().
        if (const auto panel = list.ItemsPanelRoot()) {
            for (const auto& child : panel.Children()) {
                if (const auto item = child.try_as<mux::Controls::ListViewItem>()) {
                    const int32_t row = list.IndexFromContainer(item);
                    if (row >= 0) {
                        if (const auto root = item.ContentTemplateRoot()) {
                            fill(root.as<mux::Controls::Grid>(), static_cast<std::size_t>(row));
                        }
                    }
                }
            }
        }
    }

    // --- rows -----------------------------------------------------------------

    void reload() {
        rows->reset(static_cast<uint32_t>(view.rowCount()));
        refreshHeader();
    }

    void fill(const mux::Controls::Grid& grid, std::size_t row) const {
        if (const auto tag = grid.Tag().try_as<winrt::Windows::Foundation::IReference<int32_t>>();
            !tag || tag.Value() != layout) {
            applyColumns(grid);
            grid.Tag(winrt::box_value(layout));
        }

        const PlaylistEntry* entry   = view.entryAt(row);
        const bool           current = entry != nullptr && entry->id == view.currentTrack();
        const bool           broken  = entry != nullptr && entry->error;

        const auto children = grid.Children();
        for (uint32_t p = 0; p < children.Size() && p < kColumns.size(); ++p) {
            const ColumnSpec& spec = kColumns[order[p]];
            auto              text = children.GetAt(p).as<mux::Controls::TextBlock>();
            text.Text(winrt::to_hstring(view.text(row, spec.column)));
            text.TextAlignment(spec.right ? mux::TextAlignment::Right : mux::TextAlignment::Left);
            text.FontWeight(current ? winrt::Microsoft::UI::Text::FontWeights::Bold()
                                    : winrt::Microsoft::UI::Text::FontWeights::Normal());
        }
        grid.Opacity(broken ? 0.5 : 1.0);
    }

    void refill(std::size_t row) const {
        if (const auto container = list.ContainerFromIndex(static_cast<int32_t>(row))) {
            if (auto root = container.as<mux::Controls::ListViewItem>().ContentTemplateRoot()) {
                fill(root.as<mux::Controls::Grid>(), row);
            }
        }
    }

    // --- the header -----------------------------------------------------------

    /// One cell per column: a heading, and on a resizable column a gripper
    /// straddling the gap to the next one.
    void buildHeader() {
        header = mux::Controls::Grid();
        header.ColumnSpacing(kSpacing);
        header.Padding(mux::ThicknessHelper::FromLengths(16, 6, 16, 6));
        // The rule under the headings is a ThemeResource, and only XAML can
        // say that: a brush looked up from code is the theme of the moment it
        // was looked up, and would stay light after a switch to dark.
        headerFrame = load<mux::Controls::Border>(
            L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
            L" BorderThickness='0,0,0,1'"
            L" BorderBrush='{ThemeResource DividerStrokeColorDefaultBrush}'/>");
        headerFrame.Child(header);

        for (std::size_t p = 0; p < kColumns.size(); ++p) {
            auto cell = mux::Controls::Grid();
            // Transparent, not null: a null background is not hit-testable,
            // and the whole cell is the click and drag target.
            cell.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
            mux::Controls::Grid::SetColumn(cell, static_cast<int32_t>(p));

            auto text = mux::Controls::TextBlock();
            text.FontWeight(winrt::Microsoft::UI::Text::FontWeights::SemiBold());
            text.TextTrimming(mux::TextTrimming::CharacterEllipsis);
            text.VerticalAlignment(mux::VerticalAlignment::Center);
            cell.Children().Append(text);

            mux::Controls::Grid gripper = winrt::make<Gripper>();
            gripper.Width(kSpacing);
            gripper.HorizontalAlignment(mux::HorizontalAlignment::Right);
            gripper.Margin(mux::ThicknessHelper::FromLengths(0, -6, -kSpacing, -6));
            gripper.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
            cell.Children().Append(gripper);

            cell.PointerPressed([this, p](auto const& sender, mux::Input::PointerRoutedEventArgs const& args) {
                pressHeading(sender.template as<mux::UIElement>(), p, args, false);
            });
            gripper.PointerPressed([this, p](auto const& sender, mux::Input::PointerRoutedEventArgs const& args) {
                pressHeading(sender.template as<mux::UIElement>(), p, args, true);
            });
            for (const mux::UIElement target : {cell.as<mux::UIElement>(), gripper.as<mux::UIElement>()}) {
                target.PointerMoved([this](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
                    moveHeading(args);
                });
                target.PointerReleased([this](auto const& sender, mux::Input::PointerRoutedEventArgs const& args) {
                    releaseHeading(sender.template as<mux::UIElement>(), args);
                });
                target.PointerCaptureLost([this](auto&&, auto&&) { cancelDrag(); });
            }

            header.Children().Append(cell);
        }

        // The filter's button, in the column applyColumns() adds after the
        // last heading.
        filterButton = toolButton(L"\xE71C", app::tr("Filter the playlist"));  // Segoe Fluent "Filter"
        filterButton.Click([this](auto&&, auto&&) {
            if (filterBar.Visibility() == mux::Visibility::Visible) {
                closeFilter();
            } else {
                openFilter();
            }
        });
        filterButton.VerticalAlignment(mux::VerticalAlignment::Center);
        mux::Controls::Grid::SetColumn(filterButton, static_cast<int32_t>(kColumns.size()));
        header.Children().Append(filterButton);

        applyColumns(header);
    }

    [[nodiscard]] mux::Controls::Grid headerCell(std::size_t position) const {
        return header.Children().GetAt(static_cast<uint32_t>(position)).as<mux::Controls::Grid>();
    }

    void refreshHeader() const {
        for (std::size_t p = 0; p < kColumns.size(); ++p) {
            const ColumnSpec&      spec    = kColumns[order[p]];
            const std::string_view english = PlaylistView::heading(spec.column);
            // Translated as the wx list's PlaylistColumns does it, and for the
            // same reason: core's headings are msgids, and msgid "" is the
            // catalogue's header, so the blank status heading is not looked up.
            std::string heading = english.empty() ? std::string{}
                                                  : app::tr(std::string(english).c_str());
            if (view.sortColumn() == spec.column) {
                heading += view.sortAscending() ? " \xE2\x96\xB2" : " \xE2\x96\xBC";  // ▲ ▼
            }
            const auto cell = headerCell(p);
            auto text = cell.Children().GetAt(0).as<mux::Controls::TextBlock>();
            text.Text(toH(heading));
            text.TextAlignment(spec.right ? mux::TextAlignment::Right : mux::TextAlignment::Left);
            // A gripper only on a divider that has a column to resize.
            cell.Children().GetAt(1).Visibility(resizeTarget(p) ? mux::Visibility::Visible
                                                                 : mux::Visibility::Collapsed);
        }
    }

    void pressHeading(const mux::UIElement& target, std::size_t position,
                      mux::Input::PointerRoutedEventArgs const& args, bool resizing) {
        if (!args.GetCurrentPoint(target).Properties().IsLeftButtonPressed()) {
            return;
        }
        drag            = {};
        drag.pressed    = true;
        drag.resizing   = resizing;
        drag.position   = position;
        drag.startX     = args.GetCurrentPoint(header).Position().X;
        drag.startWidth = widths[order[position]];
        if (resizing) {
            const auto resize = resizeTarget(position);
            if (!resize) {
                drag = {};
                return;
            }
            drag.target     = resize->column;
            drag.sense      = resize->sense;
            drag.startWidth = widths[resize->column];
            for (std::size_t p = 0; p < kColumns.size(); ++p) {
                if (kColumns[order[p]].fills) {
                    drag.room = std::max(0.0, headerCell(p).ActualWidth() - kMinFillWidth);
                }
            }
        }
        target.CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void moveHeading(mux::Input::PointerRoutedEventArgs const& args) {
        if (!drag.pressed) {
            return;
        }
        const double dx = args.GetCurrentPoint(header).Position().X - drag.startX;
        if (drag.resizing) {
            widths[drag.target] = std::clamp(drag.startWidth + (drag.sense * dx), kMinWidth,
                                             std::max(kMinWidth, drag.startWidth + drag.room));
            relayout();
        } else {
            if (!drag.dragging && std::abs(dx) < kDragThreshold) {
                return;
            }
            drag.dragging = true;
            // The heading follows the pointer, dimmed; the columns move when
            // it is let go.
            auto cell = headerCell(drag.position);
            cell.Opacity(0.6);
            auto shift = mux::Media::TranslateTransform();
            shift.X(dx);
            cell.RenderTransform(shift);
        }
        args.Handled(true);
    }

    void releaseHeading(const mux::UIElement& target, mux::Input::PointerRoutedEventArgs const& args) {
        if (!drag.pressed) {
            return;
        }
        const Drag done = drag;
        const double x  = args.GetCurrentPoint(header).Position().X;
        cancelDrag();
        target.ReleasePointerCapture(args.Pointer());
        args.Handled(true);

        // A release far from the press is a drag even when no move said so: the
        // moves are coalesced, and a quick flick can arrive as press, release.
        if (done.resizing) {
            persistWidths();
        } else if (done.dragging || std::abs(x - done.startX) >= kDragThreshold) {
            moveColumn(done.position, positionAt(x));
        } else {
            sortBy(kColumns[order[done.position]].column);
        }
    }

    void cancelDrag() {
        if (drag.pressed && drag.dragging) {
            auto cell = headerCell(drag.position);
            cell.Opacity(1.0);
            cell.RenderTransform(nullptr);
        }
        drag = {};
    }

    /// The display position whose heading is under x, in header coordinates.
    [[nodiscard]] std::size_t positionAt(double x) const {
        std::size_t best = kColumns.size() - 1;
        for (std::size_t p = 0; p < kColumns.size(); ++p) {
            const auto   cell = headerCell(p);
            const double left = cell.TransformToVisual(header).TransformPoint({0, 0}).X;
            if (x < left + cell.ActualWidth()) {
                best = p;
                break;
            }
        }
        return best;
    }

    void moveColumn(std::size_t from, std::size_t to) {
        if (from == to) {
            return;
        }
        const std::size_t column = order[from];
        if (from < to) {
            std::move(order.begin() + from + 1, order.begin() + to + 1, order.begin() + from);
        } else {
            std::move_backward(order.begin() + to, order.begin() + from, order.begin() + from + 1);
        }
        order[to] = column;
        relayout();
    }

    void sortBy(Column column) const {
        // Three states, as the wx header: ascending, descending, playlist order.
        if (view.sortColumn() != column) {
            view.setSort(column, true);
        } else if (view.sortAscending()) {
            view.setSort(column, false);
        } else {
            view.setSort(PlaylistView::kNoSort, true);
        }
    }

    // --- selection, menus and drops ---------------------------------------------

    [[nodiscard]] static std::optional<std::size_t> rowOf(IInspectable const& source) {
        const auto element = source.try_as<mux::FrameworkElement>();
        if (!element) {
            return std::nullopt;
        }
        // The data context the row template inherits, which is the boxed row.
        if (const auto row = element.DataContext().try_as<winrt::Windows::Foundation::IReference<int32_t>>()) {
            return static_cast<std::size_t>(row.Value());
        }
        return std::nullopt;
    }

    [[nodiscard]] bool selected(std::size_t row) const {
        for (const auto& range : list.SelectedRanges()) {
            if (static_cast<int64_t>(row) >= range.FirstIndex() &&
                static_cast<int64_t>(row) <= range.LastIndex()) {
                return true;
            }
        }
        return false;
    }

    winrt::fire_and_forget drop(winrt::Windows::ApplicationModel::DataTransfer::DataPackageView data) {
        // The table can be closed while the items are fetched; the token is
        // how the continuation finds out.
        auto weak  = std::weak_ptr<bool>(alive);
        auto items = co_await data.GetStorageItemsAsync();
        if (weak.expired()) {
            co_return;
        }
        std::vector<std::filesystem::path> paths;
        for (const auto& item : items) {
            paths.emplace_back(std::wstring_view(item.Path()));
        }
        if (!paths.empty() && owner.filesDropped) {
            owner.filesDropped(std::move(paths));
        }
    }

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

PlaylistTable::PlaylistTable(PlaylistView& view, Settings& settings)
    : impl_(std::make_unique<Impl>(*this, view, settings)) {
    {
        Impl& impl = *impl_;
        namespace dt = winrt::Windows::ApplicationModel::DataTransfer;

        impl.buildHeader();
        impl.rows = winrt::make_self<RowSource>();

        impl.list = mux::Controls::ListView();
        impl.list.SelectionMode(mux::Controls::ListViewSelectionMode::Extended);
        impl.list.ItemTemplate(load<mux::DataTemplate>(rowTemplate()));
        impl.list.ItemContainerStyle(load<mux::Style>(kItemStyle));
        impl.list.ItemsSource(impl.rows.as<IInspectable>());

        impl.list.ContainerContentChanging(
            [this](auto&&, mux::Controls::ContainerContentChangingEventArgs const& args) {
                if (args.InRecycleQueue()) {
                    return;
                }
                if (auto root = args.ItemContainer().ContentTemplateRoot()) {
                    impl_->fill(root.as<mux::Controls::Grid>(),
                                static_cast<std::size_t>(args.ItemIndex()));
                }
            });

        // Double-click and Enter play, as in the wx list.
        impl.list.DoubleTapped([this](auto&&, mux::Input::DoubleTappedRoutedEventArgs const& args) {
            if (const auto row = Impl::rowOf(args.OriginalSource()); row && rowActivated) {
                rowActivated(*row);
            }
        });
        impl.list.KeyDown([this](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter &&
                impl_->list.SelectedIndex() >= 0 && rowActivated) {
                rowActivated(static_cast<std::size_t>(impl_->list.SelectedIndex()));
                args.Handled(true);
            }
        });

        impl.list.SelectionChanged([this](auto&&, auto&&) {
            if (selectionChanged) {
                selectionChanged();
            }
        });

        // ContextRequested rather than RightTapped: it is the right-click, and
        // also Shift+F10 and the Menu key, which a right-tap handler never
        // hears. Inside the selection the menu acts on all of it; outside, the
        // selection moves to the row first.
        impl.list.ContextRequested([this](auto&&, mux::Input::ContextRequestedEventArgs const& args) {
            if (const auto row = Impl::rowOf(args.OriginalSource()); row && !impl_->selected(*row)) {
                selectOnly(*row);
            }
            if (contextMenuRequested) {
                std::optional<winrt::Windows::Foundation::Point> at;
                winrt::Windows::Foundation::Point point{};
                if (args.TryGetPosition(impl_->list, point)) {
                    at = point;
                }
                contextMenuRequested(impl_->list, at);
            }
            args.Handled(true);
        });

        // No background of its own: the table sits on whatever its host gives
        // it, which in the main window is the content layer over Mica.
        auto root = mux::Controls::Grid();
        for (const auto& height : {mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto(),
                                   mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)}) {
            auto row = mux::Controls::RowDefinition();
            row.Height(height);
            root.RowDefinitions().Append(row);
        }
        impl.buildFilter();
        mux::Controls::Grid::SetRow(impl.headerFrame, 1);
        mux::Controls::Grid::SetRow(impl.list, 2);
        root.Children().Append(impl.filterBar);
        root.Children().Append(impl.headerFrame);
        root.Children().Append(impl.list);

        // Files from Explorer, appended, as a drop on the wx list does.
        root.AllowDrop(true);
        root.DragOver([](auto&&, mux::DragEventArgs const& args) {
            if (args.DataView().Contains(dt::StandardDataFormats::StorageItems())) {
                args.AcceptedOperation(dt::DataPackageOperation::Copy);
            }
        });
        root.Drop([this](auto&&, mux::DragEventArgs const& args) {
            if (args.DataView().Contains(dt::StandardDataFormats::StorageItems())) {
                impl_->drop(args.DataView());
            }
        });

        impl.root = root;
        root.SizeChanged([this](auto&&, auto&&) {
            if (impl_->refit()) {
                impl_->relayout();
            }
        });

        impl.reload();
        impl.rebuilt    = view.rebuilt.connect([this] { impl_->reload(); });
        impl.rowChanged = view.rowChanged.connect([this](std::size_t row) { impl_->refill(row); });
    }
}

PlaylistTable::~PlaylistTable() {
    // Before the list goes: a rebuild arriving mid-teardown would reload into it.
    impl_->rebuilt.reset();
    impl_->rowChanged.reset();
    impl_->alive.reset();
}

std::vector<std::size_t> PlaylistTable::selectedRows() const {
    std::vector<std::size_t> rows;
    if (!impl_->list) {
        return rows;
    }
    // Ranges rather than SelectedItems: a select-all over a long playlist is one
    // range, not a box per row. They come back in the order they were made, so
    // they are sorted after.
    for (const auto& range : impl_->list.SelectedRanges()) {
        for (int32_t row = range.FirstIndex(); row <= range.LastIndex(); ++row) {
            rows.push_back(static_cast<std::size_t>(row));
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

bool PlaylistTable::hasSelection() const {
    return impl_->list && impl_->list.SelectedRanges().Size() > 0;
}

void PlaylistTable::selectAll() {
    if (impl_->list) {
        impl_->list.SelectAll();
    }
}

void PlaylistTable::selectOnly(std::size_t row) {
    if (!impl_->list || row >= impl_->view.rowCount()) {
        return;
    }
    impl_->list.DeselectRange(mux::Data::ItemIndexRange(0, impl_->rows->Size()));
    impl_->list.SelectRange(mux::Data::ItemIndexRange(static_cast<int32_t>(row), 1));
    reveal(row);
}

mux::UIElement PlaylistTable::element() const {
    return impl_->root;
}

void PlaylistTable::setFilter(const std::string& text) {
    impl_->filterBar.Visibility(mux::Visibility::Visible);
    impl_->filterBox.Text(toH(text));
}

void PlaylistTable::reveal(std::size_t row) {
    if (impl_->list && row < impl_->view.rowCount()) {
        impl_->list.ScrollIntoView(impl_->rows->GetAt(static_cast<uint32_t>(row)));
    }
}

}  // namespace xpcog::winui
