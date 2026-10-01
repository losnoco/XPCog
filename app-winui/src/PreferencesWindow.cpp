#include "PreferencesRows.hpp"

#include "Commands.hpp"
#include "Painting.hpp"
#include "Session.hpp"

#include "xpcog/platform/OpenUrl.hpp"

#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <exception>

namespace xpcog::winui {

using app::tr;

namespace {

namespace pickers = winrt::Microsoft::Windows::Storage::Pickers;

constexpr const wchar_t* kXmlns = L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'";

/// More than there are pages (twelve), so `pages_` never reallocates.
constexpr std::size_t kMaxPages = 32;

[[nodiscard]] bool isTrue(const std::string& text) {
    // Cog's plist stores YES/NO; Settings accepts both those and true/false.
    return text == "1" || text == "true" || text == "YES";
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

/// Lower-cased, for the search: what a page says and what was typed, compared
/// without regard to case.
[[nodiscard]] std::wstring folded(std::string_view text) {
    std::wstring wide(winrt::to_hstring(text).c_str());
    for (wchar_t& c : wide) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return wide;
}

[[nodiscard]] std::string hexOf(winrt::Windows::UI::Color colour) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", colour.R, colour.G, colour.B);
    return buffer;
}

mux::Controls::TextBlock bodyText(const std::string& text) {
    auto block = mux::Controls::TextBlock();
    block.Text(toH(text));
    block.TextWrapping(mux::TextWrapping::Wrap);
    return block;
}

}  // namespace

void Row::enable(bool enabled) const {
    if (control) {
        control.IsEnabled(enabled);
    }
    if (element) {
        element.Opacity(enabled ? 1.0 : 0.55);
    }
}

// --- the rows ------------------------------------------------------------------------

PreferencesWindow::RowBuilder::RowBuilder(Settings& settings, mux::Controls::StackPanel column,
                                          Announce announce,
                                          std::function<winrt::Microsoft::UI::WindowId()> windowId,
                                          std::string& searchText)
    : settings_(settings),
      column_(std::move(column)),
      announce_(std::move(announce)),
      windowId_(std::move(windowId)),
      searchText_(searchText) {}

void PreferencesWindow::RowBuilder::remember(std::string_view text) {
    if (!text.empty()) {
        searchText_ += text;
        searchText_ += '\n';
    }
}

void PreferencesWindow::RowBuilder::heading(const std::string& title) {
    // Windows Settings' group heading: body-strong, with air above it.
    auto block = loadXaml<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock ") + kXmlns +
        L" Style='{StaticResource BodyStrongTextBlockStyle}' Margin='1,28,0,6'/>");
    block.Text(toH(title));
    column_.Children().Append(block);
    remember(title);
}

Row PreferencesWindow::RowBuilder::card(const std::string& label, const std::string& hint,
                                        const mux::FrameworkElement& right,
                                        const mux::Controls::Control& control) {
    // A settings card: the fill and stroke Windows Settings draws its rows
    // with, title and description on the left, the control on the right.
    auto border = loadXaml<mux::Controls::Border>(
        std::wstring(L"<Border ") + kXmlns +
        L" Background='{ThemeResource CardBackgroundFillColorDefaultBrush}'"
        L" BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'"
        L" BorderThickness='1' CornerRadius='4' Padding='16,12,16,12' MinHeight='64'/>");
    auto grid = mux::Controls::Grid();
    grid.ColumnSpacing(16);
    {
        auto words = mux::Controls::ColumnDefinition();
        words.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        auto rest = mux::Controls::ColumnDefinition();
        rest.Width(mux::GridLengthHelper::Auto());
        grid.ColumnDefinitions().Append(words);
        grid.ColumnDefinitions().Append(rest);
    }
    auto words = mux::Controls::StackPanel();
    words.VerticalAlignment(mux::VerticalAlignment::Center);
    words.Children().Append(bodyText(label));
    if (!hint.empty()) {
        auto description = secondaryText(L"TextWrapping='Wrap' Style='{StaticResource CaptionTextBlockStyle}'");
        description.Text(toH(hint));
        words.Children().Append(description);
    }
    grid.Children().Append(words);
    if (right) {
        right.VerticalAlignment(mux::VerticalAlignment::Center);
        mux::Controls::Grid::SetColumn(right, 1);
        grid.Children().Append(right);
    }
    border.Child(grid);
    column_.Children().Append(border);
    if (control) {
        // The row's text names the control for a screen reader, which reads
        // the control and not the card around it.
        mux::Automation::AutomationProperties::SetName(control, toH(label));
    }
    remember(label);
    remember(hint);
    return Row{border, control};
}

Row PreferencesWindow::RowBuilder::toggle(const std::string& label, const char* key,
                                          const std::string& hint) {
    auto toggle = mux::Controls::ToggleSwitch();
    toggle.IsOn(isTrue(settings_.rawValue(key)));
    // The words are the card's; the switch's own On/Off labels would repeat
    // them less helpfully, as Windows Settings' switches do not.
    toggle.MinWidth(0);
    toggle.Toggled([this, key](auto const& sender, auto&&) {
        settings_.setRawValue(key, sender.template as<mux::Controls::ToggleSwitch>().IsOn() ? "true" : "false");
        announce_(key);
    });
    return card(label, hint, toggle, toggle);
}

Row PreferencesWindow::RowBuilder::choice(const std::string& label, const char* key,
                                          std::span<const app::Choice> choices,
                                          std::function<void(const std::string&)> onChange) {
    auto box = mux::Controls::ComboBox();
    box.MinWidth(200);
    std::vector<std::string> values;
    const std::string current = settings_.rawValue(key);
    int32_t selected = 0;
    for (const app::Choice& option : choices) {
        if (current == option.value) {
            selected = static_cast<int32_t>(values.size());
        }
        box.Items().Append(winrt::box_value(toH(tr(option.label))));
        values.emplace_back(option.value);
        remember(tr(option.label));
    }
    box.SelectedIndex(selected);
    box.SelectionChanged([this, key, values, onChange = std::move(onChange)](auto const& sender, auto&&) {
        const int32_t index = sender.template as<mux::Controls::ComboBox>().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
            return;
        }
        settings_.setRawValue(key, values[static_cast<std::size_t>(index)]);
        announce_(key);
        if (onChange) {
            onChange(values[static_cast<std::size_t>(index)]);
        }
    });
    return card(label, {}, box, box);
}

Row PreferencesWindow::RowBuilder::picker(const std::string& label, const std::vector<std::string>& names,
                                          uint32_t selected, std::function<void(uint32_t)> onChange) {
    auto box = mux::Controls::ComboBox();
    box.MinWidth(200);
    for (const std::string& name : names) {
        box.Items().Append(winrt::box_value(toH(name)));
    }
    if (selected < names.size()) {
        box.SelectedIndex(static_cast<int32_t>(selected));
    }
    box.SelectionChanged([onChange = std::move(onChange)](auto const& sender, auto&&) {
        const int32_t index = sender.template as<mux::Controls::ComboBox>().SelectedIndex();
        if (index >= 0 && onChange) {
            onChange(static_cast<uint32_t>(index));
        }
    });
    return card(label, {}, box, box);
}

Row PreferencesWindow::RowBuilder::number(const std::string& label, const char* key, double minimum,
                                          double maximum, double step, unsigned digits) {
    auto box = mux::Controls::NumberBox();
    box.Minimum(minimum);
    box.Maximum(maximum);
    box.SmallChange(step);
    box.LargeChange(step * 10);
    box.SpinButtonPlacementMode(mux::Controls::NumberBoxSpinButtonPlacementMode::Compact);
    box.ValidationMode(mux::Controls::NumberBoxValidationMode::InvalidInputOverwritten);
    box.MinWidth(160);
    auto format = winrt::Windows::Globalization::NumberFormatting::DecimalFormatter();
    format.FractionDigits(static_cast<int32_t>(digits));
    format.IsGrouped(false);
    box.NumberFormatter(format);
    box.Value(toDouble(settings_.rawValue(key)));
    box.ValueChanged([this, key, digits](auto&&, mux::Controls::NumberBoxValueChangedEventArgs const& args) {
        const double value = args.NewValue();
        if (std::isnan(value)) {
            return;  // emptied: the old value stands
        }
        settings_.setRawValue(key, digits == 0 ? std::to_string(static_cast<long long>(std::lround(value)))
                                               : std::to_string(value));
        announce_(key);
    });
    return card(label, {}, box, box);
}

Row PreferencesWindow::RowBuilder::text(const std::string& label, const char* key, bool secret) {
    // Written when the box is left or Enter is pressed: a setting that takes
    // effect per keystroke would, for an API address, try every prefix of it.
    if (secret) {
        auto box = mux::Controls::PasswordBox();
        box.Width(280);
        box.Password(toH(settings_.rawValue(key)));
        const auto commit = [this, key, box] {
            settings_.setRawValue(key, toUtf8(box.Password()));
            announce_(key);
        };
        box.LostFocus([commit](auto&&, auto&&) { commit(); });
        box.KeyDown([commit](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
                commit();
            }
        });
        return card(label, {}, box, box);
    }
    auto box = mux::Controls::TextBox();
    box.Width(280);
    box.Text(toH(settings_.rawValue(key)));
    const auto commit = [this, key, box] {
        const std::string value = toUtf8(box.Text());
        if (value != settings_.rawValue(key)) {
            settings_.setRawValue(key, value);
            announce_(key);
        }
    };
    box.LostFocus([commit](auto&&, auto&&) { commit(); });
    box.KeyDown([commit](auto&&, mux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            commit();
        }
    });
    return card(label, {}, box, box);
}

Row PreferencesWindow::RowBuilder::path(const std::string& label, const char* key, bool folders,
                                        bool files, std::vector<std::string> patterns) {
    auto buttons = mux::Controls::StackPanel();
    buttons.Orientation(mux::Controls::Orientation::Horizontal);
    buttons.Spacing(8);
    Row row = card(label, settings_.rawValue(key).empty() ? std::string(" ") : settings_.rawValue(key),
                   buttons, nullptr);

    // The description is the path; make it selectable, and keep it to update.
    auto grid  = row.element.as<mux::Controls::Border>().Child().as<mux::Controls::Grid>();
    auto words = grid.Children().GetAt(0).as<mux::Controls::StackPanel>();
    mux::Controls::TextBlock shown = words.Children().Size() > 1
                                         ? words.Children().GetAt(1).as<mux::Controls::TextBlock>()
                                         : nullptr;
    if (shown) {
        shown.IsTextSelectionEnabled(true);
    }
    const auto chosen = [this, key, shown](const winrt::hstring& path) {
        settings_.setRawValue(key, toUtf8(path));
        if (shown) {
            shown.Text(path);
        }
        announce_(key);
    };

    if (folders) {
        auto button = mux::Controls::Button();
        button.Content(winrt::box_value(toH(tr("Folder..."))));
        button.Click([this, chosen](auto&&, auto&&) -> winrt::fire_and_forget {
            pickers::FolderPicker picker(windowId_());
            try {
                if (const auto folder = co_await picker.PickSingleFolderAsync()) {
                    chosen(folder.Path());
                }
            } catch (const winrt::hresult_error&) {
            }
        });
        buttons.Children().Append(button);
    }
    if (files) {
        auto button = mux::Controls::Button();
        button.Content(winrt::box_value(toH(folders ? tr("Archive...") : tr("Choose..."))));
        button.Click([this, chosen, patterns](auto&&, auto&&) -> winrt::fire_and_forget {
            pickers::FileOpenPicker picker(windowId_());
            // "*.sf2" to ".sf2", which is how the picker spells an extension.
            for (const std::string& pattern : patterns) {
                const auto dot = pattern.find('.');
                picker.FileTypeFilter().Append(toH(dot == std::string::npos ? pattern : pattern.substr(dot)));
            }
            if (patterns.empty()) {
                picker.FileTypeFilter().Append(L"*");
            }
            try {
                if (const auto file = co_await picker.PickSingleFileAsync()) {
                    chosen(file.Path());
                }
            } catch (const winrt::hresult_error&) {
            }
        });
        buttons.Children().Append(button);
    }
    return row;
}

Row PreferencesWindow::RowBuilder::colour(const std::string& label, const char* key, const char* fallback) {
    Colour initial = parseColour(settings_.rawValue(key)).value_or(parseColour(fallback).value_or(Colour{255, 0, 0, 0}));

    auto swatch = loadXaml<mux::Controls::Border>(
        std::wstring(L"<Border ") + kXmlns +
        L" Width='40' Height='20' CornerRadius='3' BorderThickness='1'"
        L" BorderBrush='{ThemeResource ControlStrokeColorDefaultBrush}'/>");
    swatch.Background(mux::Media::SolidColorBrush(initial));

    auto picker = mux::Controls::ColorPicker();
    picker.IsAlphaEnabled(false);
    picker.IsMoreButtonVisible(false);
    picker.Color(initial);
    picker.ColorChanged([this, key, swatch](auto&&, mux::Controls::ColorChangedEventArgs const& args) {
        swatch.Background(mux::Media::SolidColorBrush(args.NewColor()));
        settings_.setRawValue(key, hexOf(args.NewColor()));
        announce_(key);
    });
    auto flyout = mux::Controls::Flyout();
    flyout.Content(picker);

    auto button = mux::Controls::Button();
    button.Content(swatch);
    button.Flyout(flyout);
    return card(label, {}, button, button);
}

Row PreferencesWindow::RowBuilder::note(const std::string& text, bool quiet) {
    auto block = quiet ? secondaryText(L"TextWrapping='Wrap' Style='{StaticResource CaptionTextBlockStyle}'")
                       : bodyText(text);
    block.Text(toH(text));
    block.Margin(mux::ThicknessHelper::FromLengths(2, 2, 0, 6));
    if (!quiet) {
        block.IsTextSelectionEnabled(true);
    }
    column_.Children().Append(block);
    remember(text);
    return Row{block, nullptr};
}

Row PreferencesWindow::RowBuilder::link(const std::string& label, std::string_view url) {
    auto icon = mux::Controls::FontIcon();
    icon.Glyph(L"\xE8A7");  // Segoe Fluent "OpenInNewWindow"
    icon.FontSize(14);
    auto button = mux::Controls::HyperlinkButton();
    button.Content(icon);
    const std::string target(url);
    button.Click([target](auto&&, auto&&) { platform::openInBrowser(target); });
    mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(toH(target)));
    return card(label, {}, button, button);
}

Row PreferencesWindow::RowBuilder::add(const std::string& label, const mux::FrameworkElement& content,
                                       const std::string& hint) {
    return card(label, hint, content, content.try_as<mux::Controls::Control>());
}

Row PreferencesWindow::RowBuilder::buttons(std::initializer_list<mux::Controls::Button> list) {
    auto strip = mux::Controls::StackPanel();
    strip.Orientation(mux::Controls::Orientation::Horizontal);
    strip.Spacing(8);
    strip.Margin(mux::ThicknessHelper::FromLengths(0, 4, 0, 4));
    for (const auto& button : list) {
        strip.Children().Append(button);
    }
    column_.Children().Append(strip);
    return Row{strip, nullptr};
}

// --- the window ----------------------------------------------------------------------

PreferencesWindow::PreferencesWindow(app::Session& session, HWND owner, bool hasTray)
    : session_(session), settings_(session.settings()), hasTray_(hasTray),
      alive_(std::make_shared<int>(0)) {
    window_ = mux::Window();
    window_.Title(toH(app::commandLabel(app::CommandId::FilePreferences)));
    window_.SystemBackdrop(mux::Media::MicaBackdrop());
    window_.ExtendsContentIntoTitleBar(true);

    auto titleBar = mux::Controls::TitleBar();
    titleBar.Title(toH(app::commandLabel(app::CommandId::FilePreferences)));

    navigation_ = mux::Controls::NavigationView();
    navigation_.PaneDisplayMode(mux::Controls::NavigationViewPaneDisplayMode::Left);
    navigation_.IsSettingsVisible(false);
    navigation_.IsBackButtonVisible(mux::Controls::NavigationViewBackButtonVisible::Collapsed);
    navigation_.IsPaneToggleButtonVisible(false);
    navigation_.OpenPaneLength(260);

    auto search = mux::Controls::AutoSuggestBox();
    search.PlaceholderText(toH(tr("Search")));
    search.QueryIcon(mux::Controls::SymbolIcon(mux::Controls::Symbol::Find));
    search.TextChanged([this](mux::Controls::AutoSuggestBox const& box, auto&&) { filter(toUtf8(box.Text())); });
    navigation_.AutoSuggestBox(search);

    content_ = mux::Controls::Grid();
    navigation_.Content(content_);
    navigation_.SelectionChanged([this](auto&&, mux::Controls::NavigationViewSelectionChangedEventArgs const& args) {
        if (const auto item = args.SelectedItem().try_as<mux::Controls::NavigationViewItem>()) {
            select(toUtf8(winrt::unbox_value<winrt::hstring>(item.Tag())));
        }
    });

    auto root = mux::Controls::Grid();
    for (const auto& height : {mux::GridLengthHelper::Auto(),
                               mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        root.RowDefinitions().Append(row);
    }
    mux::Controls::Grid::SetRow(navigation_, 1);
    root.Children().Append(titleBar);
    root.Children().Append(navigation_);
    window_.Content(root);
    window_.SetTitleBar(titleBar);

    // Room for every page up front: each builder writes the search text into
    // its page in this vector by reference, and a reallocation as the next
    // page went in would leave the earlier ones writing into freed memory.
    pages_.reserve(kMaxPages);

    // Grouped as GTK's are: what the player does, then how it sounds, then
    // the services it talks to, and Advanced -- every key, raw -- at the end.
    buildGeneralPage();
    buildPlaylistPage();
    buildAppearancePage();
    buildNotificationsPage();
    buildVisualizersPage();
    startSection(tr("Sound"));
    buildOutputPage();
    buildPitchTempoPage();
    buildMidiPage();
    startSection(tr("Services"));
    if (session_.lastFm() != nullptr && session_.scrobbler() != nullptr) {
        buildLastFmPage();
    }
    if (session_.listenBrainz() != nullptr && session_.listenBrainzScrobbler() != nullptr) {
        buildListenBrainzPage();
    }
    buildRemotePage();
    startSection({});
    buildAdvancedPage();

    // Owned by the player's window: kept above it, minimised and closed with
    // it -- the nearest a WinUI window comes to a dialog's parent.
    HWND self = nullptr;
    window_.as<IWindowNative>()->get_WindowHandle(&self);
    ::SetWindowLongPtrW(self, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    const double scale = ::GetDpiForWindow(self) / 96.0;
    window_.AppWindow().Resize({static_cast<int32_t>(1000 * scale), static_cast<int32_t>(720 * scale)});

    window_.Closed([this](auto&&, auto&&) {
        closing_ = true;
        closed.publish();
    });
}

PreferencesWindow::~PreferencesWindow() {
    subscriptions_.clear();
}

PreferencesWindow::RowBuilder* PreferencesWindow::rowsFor(const char* name, const std::string& title,
                                                          const wchar_t* glyph) {
    if (pendingSection_) {
        if (!pendingSection_->empty()) {
            auto header = mux::Controls::NavigationViewItemHeader();
            header.Content(winrt::box_value(toH(*pendingSection_)));
            navigation_.MenuItems().Append(header);
        } else {
            navigation_.MenuItems().Append(mux::Controls::NavigationViewItemSeparator());
        }
        pendingSection_.reset();
    }

    auto icon = mux::Controls::FontIcon();
    icon.Glyph(glyph);
    auto item = mux::Controls::NavigationViewItem();
    item.Content(winrt::box_value(toH(title)));
    item.Icon(icon);
    item.Tag(winrt::box_value(toH(name)));
    navigation_.MenuItems().Append(item);

    // The page: its title, then its rows, in a column of a readable width
    // that scrolls -- the shape of a page in Windows Settings.
    auto column = mux::Controls::StackPanel();
    column.Spacing(4);
    column.MaxWidth(1000);
    column.Padding(mux::ThicknessHelper::FromLengths(36, 8, 36, 36));
    auto heading = loadXaml<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock ") + kXmlns +
        L" Style='{StaticResource TitleTextBlockStyle}' Margin='0,0,0,12'/>");
    heading.Text(toH(title));
    column.Children().Append(heading);
    auto scroller = mux::Controls::ScrollViewer();
    scroller.Content(column);
    scroller.Visibility(mux::Visibility::Collapsed);
    content_.Children().Append(scroller);

    // Never past what was reserved: see the constructor.
    if (pages_.size() == pages_.capacity()) {
        std::terminate();
    }
    pages_.push_back(Page{name, item, scroller, title + "\n", nullptr});
    auto builder = std::make_shared<RowBuilder>(
        settings_, column, [this](const char* key) { settingChanged.publish(key); },
        [this] { return window_.AppWindow().Id(); }, pages_.back().text);
    builders_.push_back(builder);
    return builder.get();
}

void PreferencesWindow::select(const std::string& name) {
    for (const Page& page : pages_) {
        page.view.Visibility(page.name == name ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (page.name == name && !page.item.IsSelected()) {
            page.item.IsSelected(true);
        }
    }
}

void PreferencesWindow::filter(const std::string& query) {
    // A page is listed when its title or any of its rows mention what was
    // typed, as in GTK's sidebar; the headers stay, the rule says where.
    const std::wstring wanted = folded(query);
    for (const Page& page : pages_) {
        const bool shown = wanted.empty() || folded(page.text).find(wanted) != std::wstring::npos;
        page.item.Visibility(shown ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
}

void PreferencesWindow::show(std::optional<PreferencesPage> page) {
    std::string name = pages_.empty() ? std::string{} : pages_.front().name;
    if (page) {
        switch (*page) {
            case PreferencesPage::Playlist:
                name = "playlist";
                break;
            case PreferencesPage::PitchTempo:
                name = "pitch-tempo";
                break;
            case PreferencesPage::Visualizers:
                name = "visualizers";
                break;
        }
    } else {
        // Where it was left, or the first page the first time.
        for (const Page& candidate : pages_) {
            if (candidate.view.Visibility() == mux::Visibility::Visible) {
                name = candidate.name;
            }
        }
    }
    select(name);
    window_.Activate();
}

void PreferencesWindow::close() {
    if (!closing_) {
        window_.Close();
    }
}

}  // namespace xpcog::winui
