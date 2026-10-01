#include "MainWindow.hpp"

#include "Session.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace xpcog::winui {

namespace {

// Segoe Fluent Icons code points: the system's own glyphs for these, the same
// ones Media Player draws.
constexpr const wchar_t* kGlyphPlay     = L"\xE768";
constexpr const wchar_t* kGlyphPause    = L"\xE769";
constexpr const wchar_t* kGlyphStop     = L"\xE71A";
constexpr const wchar_t* kGlyphPrevious = L"\xE892";
constexpr const wchar_t* kGlyphNext     = L"\xE893";
constexpr const wchar_t* kGlyphVolume   = L"\xE767";

constexpr const wchar_t* kXmlns = L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'";

template <typename T>
T load(const std::wstring& xaml) {
    return mux::Markup::XamlReader::Load(xaml).as<T>();
}

/// The seek bar's thumb tooltip: the position as a clock rather than as the
/// slider's bare number of seconds.
struct ClockConverter : winrt::implements<ClockConverter, mux::Data::IValueConverter> {
    winrt::Windows::Foundation::IInspectable Convert(
        winrt::Windows::Foundation::IInspectable const& value,
        winrt::Windows::UI::Xaml::Interop::TypeName const&,
        winrt::Windows::Foundation::IInspectable const&, winrt::hstring const&) const {
        return winrt::box_value(toH(app::formatClock(winrt::unbox_value_or<double>(value, 0.0))));
    }
    winrt::Windows::Foundation::IInspectable ConvertBack(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Windows::UI::Xaml::Interop::TypeName const&,
        winrt::Windows::Foundation::IInspectable const&, winrt::hstring const&) const {
        throw winrt::hresult_not_implemented();
    }
};

std::filesystem::path besideExecutable(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / name;
}

}  // namespace

MainWindow::MainWindow(app::Session& session) : session_(session) {
    build();
    wireUp();
}

MainWindow::~MainWindow() {
    // Before the controls: a signal arriving now would reach a dead window.
    subscriptions_.clear();
}

HWND MainWindow::hwnd() const {
    HWND handle = nullptr;
    window_.as<IWindowNative>()->get_WindowHandle(&handle);
    return handle;
}

void MainWindow::activate() {
    window_.Activate();
}

// --- building -------------------------------------------------------------------

void MainWindow::build() {
    window_ = mux::Window();
    window_.Title(L"XPCog");

    // Mica, the backdrop for a long-lived main window. It falls back to the
    // theme's solid colour by itself where it cannot be drawn -- Windows 10,
    // Remote Desktop, transparency effects turned off -- so nothing here asks.
    window_.SystemBackdrop(mux::Media::MicaBackdrop());

    // The title bar is ours, so Mica runs up under it: the TitleBar control
    // draws the icon and title, keeps the caption buttons' space and drag
    // region, and lets interactive content -- the filter -- sit in it.
    window_.ExtendsContentIntoTitleBar(true);

    const std::filesystem::path icon = besideExecutable(L"xpcog.ico");
    window_.AppWindow().SetIcon(icon.wstring());

    titleBar_ = mux::Controls::TitleBar();
    titleBar_.Title(L"XPCog");
    {
        auto source = mux::Controls::ImageIconSource();
        source.ImageSource(mux::Media::Imaging::BitmapImage(
            winrt::Windows::Foundation::Uri(L"file:///" + icon.generic_wstring())));
        titleBar_.IconSource(source);
    }

    auto filter = mux::Controls::AutoSuggestBox();
    filter.PlaceholderText(toH(app::tr("Filter")));
    filter.QueryIcon(mux::Controls::SymbolIcon(mux::Controls::Symbol::Find));
    filter.MinWidth(240);
    filter.MaxWidth(420);
    filter.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    filter.VerticalAlignment(mux::VerticalAlignment::Center);
    filter.TextChanged([this](mux::Controls::AutoSuggestBox const& box, auto&&) {
        session_.view().setFilter(toUtf8(box.Text()));
    });
    titleBar_.Content(filter);

    // --- the transport row ---------------------------------------------------
    auto transport = mux::Controls::Grid();
    transport.ColumnSpacing(8);
    transport.Padding(mux::ThicknessHelper::FromLengths(12, 4, 16, 8));
    for (const auto& width : {mux::GridLengthHelper::Auto(),
                              mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                              mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto(),
                              mux::GridLengthHelper::Auto()}) {
        auto column = mux::Controls::ColumnDefinition();
        column.Width(width);
        transport.ColumnDefinitions().Append(column);
    }

    auto buttons = mux::Controls::StackPanel();
    buttons.Orientation(mux::Controls::Orientation::Horizontal);
    buttons.Spacing(2);
    buttons.Children().Append(transportButton(kGlyphPrevious, app::tr("Previous"),
                                              [this] { session_.playback().previous(); }));
    playButton_ = transportButton(kGlyphPlay, app::tr("Play"),
                                  [this] { session_.playback().playPause(); });
    playGlyph_ = playButton_.Content().as<mux::Controls::FontIcon>();
    buttons.Children().Append(playButton_);
    buttons.Children().Append(transportButton(kGlyphStop, app::tr("Stop"),
                                              [this] { session_.playback().stop(); }));
    buttons.Children().Append(transportButton(kGlyphNext, app::tr("Next"),
                                              [this] { session_.playback().next(); }));
    transport.Children().Append(buttons);

    seek_ = mux::Controls::Slider();
    seek_.Minimum(0);
    seek_.Maximum(1);
    seek_.StepFrequency(0.1);
    seek_.IsEnabled(false);
    seek_.VerticalAlignment(mux::VerticalAlignment::Center);
    seek_.Margin(mux::ThicknessHelper::FromLengths(8, 0, 4, 0));
    seek_.ThumbToolTipValueConverter(winrt::make<ClockConverter>());
    mux::Controls::Grid::SetColumn(seek_, 1);
    transport.Children().Append(seek_);

    clock_ = load<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock ") + kXmlns +
        L" VerticalAlignment='Center' MinWidth='96' TextAlignment='Center'"
        L" Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>");
    mux::Controls::Grid::SetColumn(clock_, 2);
    setClock(0, 0);
    transport.Children().Append(clock_);

    auto speaker = mux::Controls::FontIcon();
    speaker.Glyph(kGlyphVolume);
    speaker.FontSize(16);
    speaker.VerticalAlignment(mux::VerticalAlignment::Center);
    mux::Controls::Grid::SetColumn(speaker, 3);
    transport.Children().Append(speaker);

    volume_ = mux::Controls::Slider();
    volume_.Minimum(0);
    volume_.Maximum(100);
    volume_.Width(120);
    volume_.VerticalAlignment(mux::VerticalAlignment::Center);
    volume_.Value(session_.settings().Volume() * 100.0);
    mux::Controls::ToolTipService::SetToolTip(volume_, winrt::box_value(toH(app::tr("Volume"))));
    mux::Controls::Grid::SetColumn(volume_, 4);
    transport.Children().Append(volume_);

    // --- the playlist, on the content layer -------------------------------------
    //
    // A card: LayerFillColorDefault over Mica, with the card stroke and corner
    // radius the design guidance gives the content layer. ThemeResource, so it
    // follows a switch between light and dark while the window is open.
    auto card = load<mux::Controls::Border>(
        std::wstring(L"<Border ") + kXmlns +
        L" Background='{ThemeResource LayerFillColorDefaultBrush}'"
        L" BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'"
        L" BorderThickness='1' CornerRadius='8' Margin='12,0,12,0'/>");
    playlist_ = std::make_unique<PlaylistTable>(session_.view(), session_.settings());
    card.Child(playlist_->element());

    status_ = load<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock ") + kXmlns +
        L" Margin='16,6,16,8' TextTrimming='CharacterEllipsis'"
        L" Style='{StaticResource CaptionTextBlockStyle}'"
        L" Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>");

    // --- the window ------------------------------------------------------------
    //
    // No Background anywhere on this tree: an opaque one would cover the Mica.
    auto root = mux::Controls::Grid();
    for (const auto& height : {mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto(),
                               mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                               mux::GridLengthHelper::Auto()}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        root.RowDefinitions().Append(row);
    }
    mux::Controls::Grid::SetRow(transport, 1);
    mux::Controls::Grid::SetRow(card, 2);
    mux::Controls::Grid::SetRow(status_, 3);
    root.Children().Append(titleBar_);
    root.Children().Append(transport);
    root.Children().Append(card);
    root.Children().Append(status_);
    window_.Content(root);
    window_.SetTitleBar(titleBar_);

    // A first size, in physical pixels, scaled from the 96-DPI one. Remembering
    // the last size and place is the window-state step, not this one.
    const double scale = ::GetDpiForWindow(hwnd()) / 96.0;
    window_.AppWindow().Resize({static_cast<int32_t>(1280 * scale), static_cast<int32_t>(820 * scale)});
}

mux::Controls::Button MainWindow::transportButton(const wchar_t* glyph, const std::string& tooltip,
                                                  std::function<void()> action) {
    auto icon = mux::Controls::FontIcon();
    icon.Glyph(glyph);
    icon.FontSize(16);

    // A subtle button: no fill and no border at rest, the theme's hover and
    // pressed fills when touched. Transparent rather than null, because the
    // template only swaps its own resources in for the pointer states.
    auto button = mux::Controls::Button();
    button.Content(icon);
    button.Width(40);
    button.Height(36);
    button.Padding(mux::ThicknessHelper::FromUniformLength(0));
    button.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    button.BorderBrush(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    mux::Controls::ToolTipService::SetToolTip(button, winrt::box_value(toH(tooltip)));
    // The tooltip is not an accessible name; a glyph button needs one.
    mux::Automation::AutomationProperties::SetName(button, toH(tooltip));
    button.Click([action = std::move(action)](auto&&, auto&&) { action(); });
    return button;
}

// --- wiring ---------------------------------------------------------------------

void MainWindow::wireUp() {
    const auto observe = [this](auto& signal, auto handler) {
        subscriptions_.push_back(signal.connect(std::move(handler)));
    };

    observe(session_.status, [this](const std::string& text) { status_.Text(toH(text)); });
    observe(session_.trackChanged,
            [this](TrackId, const PlaylistEntry* entry, bool) { onTrackChanged(entry); });
    observe(session_.playbackStateChanged,
            [this](bool playing, bool paused) { onPlaybackStateChanged(playing, paused); });
    observe(session_.positionChanged,
            [this](double seconds, double duration) { onPositionChanged(seconds, duration); });
    observe(session_.volumeChanged, [this](double gain) {
        settingVolume_ = true;
        volume_.Value(gain * 100.0);
        settingVolume_ = false;
    });
    observe(session_.revealRequested, [this](TrackId id) {
        if (const auto row = session_.view().rowForTrack(id)) {
            playlist_->selectOnly(*row);
        }
    });
    observe(session_.tracksUpdated, [this] { status_.Text(toH(session_.statusSummary())); });

    playlist_->rowActivated = [this](std::size_t row) {
        if (const TrackId id = session_.view().trackAt(row); id != kInvalidTrackId) {
            session_.playback().playTrack(id);
        }
    };
    playlist_->filesDropped = [this](std::vector<std::filesystem::path> paths) {
        std::vector<Url> urls;
        urls.reserve(paths.size());
        for (const std::filesystem::path& path : paths) {
            urls.push_back(Url::fromLocalPath(path));
        }
        session_.addUrls(urls);
    };

    volume_.ValueChanged([this](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        if (!settingVolume_) {
            session_.setVolume(args.NewValue() / 100.0);
        }
    });

    // The seek bar seeks on release, not on every step of a drag: a seek is a
    // decoder reopen for some formats, and dragging would queue dozens. The
    // Slider handles its own pointer events, so these listen with
    // handledEventsToo to hear about them at all.
    seek_.AddHandler(mux::UIElement::PointerPressedEvent(),
                     winrt::box_value(mux::Input::PointerEventHandler(
                         [this](auto&&, auto&&) { scrubbing_ = true; })),
                     true);
    const auto release = [this](auto&&, auto&&) {
        if (scrubbing_) {
            scrubbing_ = false;
            session_.playback().seek(seek_.Value());
        }
    };
    seek_.AddHandler(mux::UIElement::PointerReleasedEvent(),
                     winrt::box_value(mux::Input::PointerEventHandler(release)), true);
    seek_.AddHandler(mux::UIElement::PointerCaptureLostEvent(),
                     winrt::box_value(mux::Input::PointerEventHandler(release)), true);
    seek_.ValueChanged([this](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        if (settingSeek_) {
            return;
        }
        if (scrubbing_) {
            // Where the release would land, while the pointer is down.
            setClock(args.NewValue(), duration_);
        } else {
            // The keyboard: arrows and Page Up/Down move the thumb with no
            // pointer involved, and seek as they go.
            session_.playback().seek(args.NewValue());
        }
    });

    window_.Closed([this](auto&&, auto&&) {
        if (closed) {
            closed();
        }
    });

    status_.Text(toH(session_.lastStatus().empty() ? session_.statusSummary()
                                                   : session_.lastStatus()));
}

// --- what playback reports --------------------------------------------------------

void MainWindow::onTrackChanged(const PlaylistEntry* entry) {
    const std::string text = entry != nullptr ? entry->display() : std::string{};
    // The track first in the window's own title, which is what the taskbar
    // shows; the same convention, and the same untranslated dash, as the wx
    // frame's (see MainFrame::onTrackChanged).
    window_.Title(text.empty() ? winrt::hstring(L"XPCog") : toH(text + " \xE2\x80\x94 XPCog"));
    titleBar_.Subtitle(toH(text));
}

void MainWindow::onPlaybackStateChanged(bool playing, bool paused) {
    const bool showsPause = playing && !paused;
    playGlyph_.Glyph(showsPause ? kGlyphPause : kGlyphPlay);
    const winrt::hstring label = toH(app::tr(showsPause ? "Pause" : "Play"));
    mux::Controls::ToolTipService::SetToolTip(playButton_, winrt::box_value(label));
    mux::Automation::AutomationProperties::SetName(playButton_, label);
    if (!playing) {
        onPositionChanged(0, 0);
    }
}

void MainWindow::onPositionChanged(double seconds, double duration) {
    duration_ = duration;
    seek_.IsEnabled(duration > 0);
    if (scrubbing_) {
        return;
    }
    settingSeek_ = true;
    seek_.Maximum(std::max(duration, 1.0));
    seek_.Value(std::clamp(seconds, 0.0, std::max(duration, 1.0)));
    settingSeek_ = false;
    setClock(seconds, duration);
}

void MainWindow::setClock(double seconds, double duration) {
    clock_.Text(toH(app::formatClock(seconds) + " / " + app::formatClock(duration)));
}

}  // namespace xpcog::winui
