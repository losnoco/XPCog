// The playlist as a WinUI ListView: the half of the prototype that asks whether
// WinUI can carry the one control this player cannot do without.
//
// WinUI has no multi-column table for C++ -- the Community Toolkit DataGrid is
// C# only -- so this is a ListView whose rows are a Grid of TextBlocks, and a
// header Grid above it with the same column definitions. Both are built from
// XAML text with XamlReader, which needs no XAML compiler.
//
// Rows are filled in ContainerContentChanging rather than bound. That is the
// fast path the ListView documentation recommends for large lists anyway, and
// here it is also the only reasonable one: {x:Bind} needs the compiler, and
// {Binding} on a C++/WinRT object needs ICustomPropertyProvider for every row.
// Each item is just its row number; the text comes from PlaylistView at the
// moment a container is about to be shown, so only the visible rows are ever
// formatted.

#include "WinUIHost.hpp"

#include "xpcog/core/library/PlaylistView.hpp"

#include <wx/translation.h>

#include <array>
#include <string>
#include <vector>

namespace xpcog::app {

namespace {

using Column = PlaylistView::Column;

struct ColumnSpec {
    Column         column;
    const wchar_t* width;  ///< a XAML GridLength
    bool           right;  ///< right-aligned, for the numbers
};

// Proportional rather than draggable: resizable columns would be a header of
// our own with splitters and widths kept in two places, which is work for the
// port rather than for the question this answers.
constexpr std::array kColumns{
    ColumnSpec{Column::Status, L"24", false}, ColumnSpec{Column::Track, L"40", true},
    ColumnSpec{Column::Title, L"3*", false},  ColumnSpec{Column::Artist, L"2*", false},
    ColumnSpec{Column::Album, L"2*", false},  ColumnSpec{Column::Length, L"56", true},
};

constexpr const wchar_t* kXmlns =
    L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'";

std::wstring columnDefinitions() {
    std::wstring xaml = L"<Grid.ColumnDefinitions>";
    for (const ColumnSpec& spec : kColumns) {
        xaml += L"<ColumnDefinition Width='";
        xaml += spec.width;
        xaml += L"'/>";
    }
    xaml += L"</Grid.ColumnDefinitions>";
    return xaml;
}

std::wstring cells(const wchar_t* extra) {
    std::wstring xaml;
    for (std::size_t i = 0; i < kColumns.size(); ++i) {
        xaml += L"<TextBlock Grid.Column='" + std::to_wstring(i) +
                L"' VerticalAlignment='Center' TextTrimming='CharacterEllipsis'";
        if (kColumns[i].right) {
            xaml += L" TextAlignment='Right'";
        }
        xaml += extra;
        xaml += L"/>";
    }
    return xaml;
}

// The row and the header share the column definitions and the spacing, which
// is what keeps the columns under their headings. The header's horizontal
// padding is the item container's, set in the style below.
std::wstring rowTemplate() {
    return std::wstring(L"<DataTemplate ") + kXmlns + L"><Grid ColumnSpacing='12'>" +
           columnDefinitions() + cells(L"") + L"</Grid></DataTemplate>";
}

std::wstring headerXaml() {
    return std::wstring(L"<Grid ") + kXmlns +
           L" ColumnSpacing='12' Padding='16,6,16,6'"
           L" BorderThickness='0,0,0,1'"
           L" BorderBrush='{ThemeResource DividerStrokeColorDefaultBrush}'>" +
           columnDefinitions() + cells(L" FontWeight='SemiBold'") + L"</Grid>";
}

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

template <typename T>
T load(const std::wstring& xaml) {
    return mux::Markup::XamlReader::Load(xaml).as<T>();
}

}  // namespace

struct WinUIPlaylist::Impl {
    explicit Impl(PlaylistView& v) : view(v) {}

    PlaylistView&                                                              view;
    mux::Controls::ListView                                                    list{nullptr};
    mux::Controls::Grid                                                        header{nullptr};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> rows{nullptr};

    Subscription rebuilt;
    Subscription rowChanged;

    void reload() {
        // One boxed integer per row. Cheap enough for a prototype into the
        // hundreds of thousands; the real thing would be a virtual vector with
        // IItemsRangeInfo, which holds nothing for rows that are not on screen.
        std::vector<winrt::Windows::Foundation::IInspectable> items;
        items.reserve(view.rowCount());
        for (std::size_t row = 0; row < view.rowCount(); ++row) {
            items.push_back(winrt::box_value(static_cast<int32_t>(row)));
        }
        rows.ReplaceAll(items);
        refreshHeader();
    }

    void fill(const mux::Controls::Grid& grid, std::size_t row) const {
        const PlaylistEntry* entry   = view.entryAt(row);
        const bool           current = entry != nullptr && entry->id == view.currentTrack();
        const bool           broken  = entry != nullptr && entry->error;

        const auto children = grid.Children();
        for (uint32_t i = 0; i < children.Size() && i < kColumns.size(); ++i) {
            auto text = children.GetAt(i).as<mux::Controls::TextBlock>();
            text.Text(winrt::to_hstring(view.text(row, kColumns[i].column)));
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

    void refreshHeader() const {
        const auto children = header.Children();
        for (uint32_t i = 0; i < children.Size() && i < kColumns.size(); ++i) {
            const Column           column  = kColumns[i].column;
            const std::string_view english = PlaylistView::heading(column);
            // Translated as PlaylistColumns does it, and for the same reason:
            // core's headings are msgids, and msgid "" is the catalogue header.
            wxString heading = english.empty()
                                   ? wxString{}
                                   : wxGetTranslation(wxString::FromUTF8(english.data(),
                                                                         english.size()));
            if (view.sortColumn() == column) {
                heading += view.sortAscending() ? L" ▲" : L" ▼";
            }
            children.GetAt(i).as<mux::Controls::TextBlock>().Text(
                winrt::hstring(heading.ToStdWstring()));
        }
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
};

WinUIPlaylist::WinUIPlaylist(wxWindow* parent, PlaylistView& view)
    : WinUIIsland(parent), impl_(std::make_unique<Impl>(view)) {
    if (!ok()) {
        return;
    }

    try {
        Impl& impl = *impl_;

        impl.header = load<mux::Controls::Grid>(headerXaml());
        const auto headings = impl.header.Children();
        for (uint32_t i = 0; i < headings.Size() && i < kColumns.size(); ++i) {
            const Column column = kColumns[i].column;
            headings.GetAt(i).Tapped(
                [this, column](auto&&, auto&&) { impl_->sortBy(column); });
        }

        impl.rows = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();

        impl.list = mux::Controls::ListView();
        impl.list.SelectionMode(mux::Controls::ListViewSelectionMode::Extended);
        impl.list.ItemTemplate(load<mux::DataTemplate>(rowTemplate()));
        impl.list.ItemContainerStyle(load<mux::Style>(kItemStyle));
        impl.list.ItemsSource(impl.rows);

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

        // Double-click plays, as in the wx list. The row comes from the data
        // context the template inherits, which is the boxed row number.
        impl.list.DoubleTapped([this](auto&&, mux::Input::DoubleTappedRoutedEventArgs const& args) {
            auto element = args.OriginalSource().try_as<mux::FrameworkElement>();
            if (!element) {
                return;
            }
            if (auto row = element.DataContext().try_as<winrt::Windows::Foundation::IReference<int32_t>>()) {
                if (rowActivated) {
                    rowActivated(static_cast<std::size_t>(row.Value()));
                }
            }
        });
        impl.list.KeyDown([this](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter &&
                impl_->list.SelectedIndex() >= 0 && rowActivated) {
                rowActivated(static_cast<std::size_t>(impl_->list.SelectedIndex()));
                args.Handled(true);
            }
        });

        auto root = mux::Controls::Grid();
        root.Background(mux::Media::SolidColorBrush(winrt::unbox_value<winrt::Windows::UI::Color>(
            mux::Application::Current().Resources().Lookup(
                winrt::box_value(L"SolidBackgroundFillColorBase")))));
        auto auto_ = mux::Controls::RowDefinition();
        auto_.Height(mux::GridLengthHelper::Auto());
        auto star = mux::Controls::RowDefinition();
        star.Height(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        root.RowDefinitions().Append(auto_);
        root.RowDefinitions().Append(star);
        mux::Controls::Grid::SetRow(impl.list, 1);
        root.Children().Append(impl.header);
        root.Children().Append(impl.list);
        host_->source.Content(root);

        impl.reload();
        impl.rebuilt    = view.rebuilt.connect([this] { impl_->reload(); });
        impl.rowChanged = view.rowChanged.connect([this](std::size_t row) { impl_->refill(row); });
    } catch (const winrt::hresult_error& error) {
        fail(describeWinRTError("The playlist island did not build", error));
    }
}

WinUIPlaylist::~WinUIPlaylist() {
    // Before the list goes: a rebuild arriving mid-teardown would reload into it.
    impl_->rebuilt.reset();
    impl_->rowChanged.reset();
}

void WinUIPlaylist::reveal(std::size_t row) {
    if (impl_->list && row < impl_->view.rowCount()) {
        impl_->list.ScrollIntoView(impl_->rows.GetAt(static_cast<uint32_t>(row)));
    }
}

}  // namespace xpcog::app
