#include "MainWindow.hpp"

#include "Session.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/Version.hpp"

#include <winrt/Microsoft.Windows.Storage.Pickers.h>

#include <microsoft.ui.xaml.window.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// How far the sizers let the panes go, in DIPs. Narrower than the minimums
// and a pane stops being able to show what it is for; wider than the maximums
// and it only crowds the playlist.
constexpr double kMinTreeWidth     = 180;
constexpr double kMaxTreeWidth     = 640;
constexpr double kMinPanelWidth    = 260;
constexpr double kMaxPanelWidth    = 720;
constexpr double kMinPlaylistWidth = 320;
/// The playing track at the title bar's right end: room for most names, and
/// the rest of the bar left to the menus.
constexpr double kMaxTrackTextWidth = 420;
constexpr double kMinStripHeight   = 120;
constexpr double kMinContentHeight = 160;

constexpr const wchar_t* kXmlns = L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'";

template <typename T>
T load(const std::wstring& xaml) {
    return mux::Markup::XamlReader::Load(xaml).as<T>();
}

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

void MainWindow::raise() {
    if (const auto presenter = window_.AppWindow().Presenter().try_as<
            winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter && presenter.State() == winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Minimized) {
        presenter.Restore();
    }
    window_.Activate();
    // Allowed because the launch that asked for this gave its permission
    // first (platform::permitForegroundHandover); without it Windows only
    // flashes the taskbar button.
    ::SetForegroundWindow(hwnd());
}

void MainWindow::activate() {
    window_.Activate();
    // After it is shown: maximising a window that has not been shown yet
    // shows it, and before Activate that is a flash of the wrong size.
    if (maximizeOnShow_) {
        if (const auto presenter = window_.AppWindow().Presenter().try_as<
                winrt::Microsoft::UI::Windowing::OverlappedPresenter>()) {
            presenter.Maximize();
        }
        maximizeOnShow_ = false;
    }
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
    // region, and lets interactive content sit in it -- here the menu bar,
    // and the playing track at its right end. Tall, so the caption buttons
    // are as tall as the menu bar beside them.
    window_.ExtendsContentIntoTitleBar(true);
    window_.AppWindow().TitleBar().PreferredHeightOption(
        winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Tall);

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

    // The playing track, at the title bar's right end (onTrackChanged): there
    // a name of any length moves nothing else, where a subtitle would push
    // the menu bar along.
    trackText_ = load<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock ") + kXmlns +
        L" VerticalAlignment='Center' TextTrimming='CharacterEllipsis' TextWrapping='NoWrap'"
        L" Margin='8,0,8,0' Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>");
    trackText_.MaxWidth(kMaxTrackTextWidth);
    titleBar_.RightHeader(trackText_);

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

    // A slider, or the waveform drawn with Win2D -- see SeekBar.hpp.
    seekBar_ = std::make_unique<SeekBar>();
    {
        auto bar = seekBar_->element().as<mux::FrameworkElement>();
        bar.VerticalAlignment(mux::VerticalAlignment::Center);
        bar.Margin(mux::ThicknessHelper::FromLengths(8, 0, 4, 0));
        mux::Controls::Grid::SetColumn(bar, 1);
        transport.Children().Append(bar);
    }

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
    // On a card -- see card() in WinRT.hpp -- as every region holding content is.
    playlist_ = std::make_unique<PlaylistTable>(session_.view(), session_.settings());
    auto playlistCard = card();
    playlistCard.Child(playlist_->element());

    // --- the panes, as GTK lays them out -------------------------------------------
    //
    // The folders on the left, Info and Lyrics on the right, the tools in a
    // strip along the bottom: fixed places, shown and hidden from the View
    // menu, and each its own card on the Mica. No docking or floating -- the
    // GTK player's decision, and for the same reason: nothing in the toolkit
    // manages a dock, and a layout that cannot be lost needs no "reset".
    fileTree_ = std::make_unique<FileTreePane>(session_.registry(),
                                               [this] { return window_.AppWindow().Id(); });
    treeCard_ = card();
    treeCard_.Width(280);
    treeCard_.Child(fileTree_->element());
    treeCard_.Visibility(mux::Visibility::Collapsed);

    info_   = std::make_unique<InfoPane>(session_.library(),
                                       [this] { return window_.Content().XamlRoot(); });
    lyrics_ = std::make_unique<LyricsPane>([this] { return session_.playback().position(); });
    lyrics_->setLookup(session_.lyricsLookup());
    lyrics_->setTimed(session_.settings().LyricsSynced());
    lyrics_->timedToggled = [this] { onCommand(app::CommandId::ViewTimedLyrics); };

    panelSelector_ = mux::Controls::SelectorBar();
    for (const auto& [page, id] : {std::pair{"info", app::CommandId::ViewInfo},
                                   std::pair{"lyrics", app::CommandId::ViewLyrics}}) {
        auto item = mux::Controls::SelectorBarItem();
        item.Text(toH(app::commandLabel(id)));
        item.Tag(winrt::box_value(toH(page)));
        panelSelector_.Items().Append(item);
    }
    panelSelector_.SelectionChanged([this](auto&&, auto&&) {
        if (const auto item = panelSelector_.SelectedItem()) {
            showPanelPage(toUtf8(winrt::unbox_value<winrt::hstring>(item.Tag())));
        }
    });
    auto panelBody = mux::Controls::Grid();
    for (const auto& height : {mux::GridLengthHelper::Auto(),
                               mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star)}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        panelBody.RowDefinitions().Append(row);
    }
    panelSelector_.Margin(mux::ThicknessHelper::FromLengths(8, 4, 8, 0));
    panelBody.Children().Append(panelSelector_);
    for (const mux::UIElement page : {info_->element(), lyrics_->element()}) {
        mux::Controls::Grid::SetRow(page.as<mux::FrameworkElement>(), 1);
        panelBody.Children().Append(page);
    }
    panelCard_ = card();
    panelCard_.Width(360);
    panelCard_.Child(panelBody);
    panelCard_.Visibility(mux::Visibility::Collapsed);
    showPanelPage("info");

    spectrum_  = std::make_unique<SpectrumView>(session_.playback().tap(), session_.settings());
    scope_     = std::make_unique<OscilloscopeView>(session_.playback().tap(), session_.settings());
#ifdef XPCOG_HAVE_SC55_PANEL
    sc55_ = std::make_unique<Sc55View>([this] { return session_.playback().position(); });
#endif
    equalizer_ = std::make_unique<EqualizerPane>(session_.settings());
    speed_     = std::make_unique<SpeedPane>(session_.settings());
    tools_     = std::make_unique<ToolsStrip>();
    tools_->addSection("spectrum", app::commandLabel(app::CommandId::ViewSpectrum),
                       spectrum_->element(), ToolsStrip::Scroll::None, ToolsStrip::Chrome::Dark);
    tools_->addSection("scope", app::commandLabel(app::CommandId::ViewOscilloscope),
                       scope_->element(), ToolsStrip::Scroll::None, ToolsStrip::Chrome::Dark);
    tools_->addSection("equalizer", app::commandLabel(app::CommandId::ViewEqualizer),
                       equalizer_->element(), ToolsStrip::Scroll::Both);
    tools_->addSection("speed", app::commandLabel(app::CommandId::ViewSpeed), speed_->element(),
                       ToolsStrip::Scroll::Vertical);
#ifdef XPCOG_HAVE_SC55_PANEL
    tools_->addSection("sc55", app::commandLabel(app::CommandId::ViewSc55Panel), sc55_->element(),
                       ToolsStrip::Scroll::None, ToolsStrip::Chrome::Orange);
#endif
    tools_->closeRequested = [this](const std::string& name) { showTool(name, false); };

    // The gaps between the cards are the sizers: columns tree | gap | playlist
    // | gap | panel, rows content | gap | tools. A gap whose pane is hidden
    // goes with it, so a hidden pane leaves no handle and no space behind.
    middle_ = mux::Controls::Grid();
    auto& middle = middle_;
    middle.Margin(mux::ThicknessHelper::FromLengths(12, 0, 12, 0));
    for (const auto& width : {mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto(),
                              mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                              mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto()}) {
        auto column = mux::Controls::ColumnDefinition();
        column.Width(width);
        middle.ColumnDefinitions().Append(column);
    }
    // The playlist keeps a usable width however far the panes are dragged.
    middle.ColumnDefinitions().GetAt(2).MinWidth(kMinPlaylistWidth);
    for (const auto& height : {mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                               mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto()}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        middle.RowDefinitions().Append(row);
    }

    treeSizer_ = std::make_unique<Sizer>(
        Sizer::Axis::Columns, [this] { return treeCard_.Width(); },
        [this](double width) { treeCard_.Width(std::clamp(width, kMinTreeWidth, kMaxTreeWidth)); },
        +1.0);
    panelSizer_ = std::make_unique<Sizer>(
        Sizer::Axis::Columns, [this] { return panelCard_.Width(); },
        [this](double width) { panelCard_.Width(std::clamp(width, kMinPanelWidth, kMaxPanelWidth)); },
        -1.0);
    toolsSizer_ = std::make_unique<Sizer>(
        Sizer::Axis::Rows, [this] { return toolsHost_.ActualHeight(); },
        [this](double height) {
            // Up to whatever leaves the playlist a few rows to show.
            const double most = std::max(kMinStripHeight, middle_.ActualHeight() - kMinContentHeight);
            toolsHost_.Height(std::clamp(height, kMinStripHeight, most));
        },
        -1.0);
    for (Sizer* sizer : {treeSizer_.get(), panelSizer_.get(), toolsSizer_.get()}) {
        sizer->finished = [this] { persistState(); };
    }

    mux::Controls::Grid::SetColumn(treeSizer_->element(), 1);
    mux::Controls::Grid::SetColumn(playlistCard, 2);
    mux::Controls::Grid::SetColumn(panelSizer_->element(), 3);
    mux::Controls::Grid::SetColumn(panelCard_, 4);
    toolsHost_ = tools_->element().as<mux::FrameworkElement>();
    mux::Controls::Grid::SetRow(toolsSizer_->element(), 1);
    mux::Controls::Grid::SetColumnSpan(toolsSizer_->element(), 5);
    mux::Controls::Grid::SetRow(toolsHost_, 2);
    mux::Controls::Grid::SetColumnSpan(toolsHost_, 5);
    toolsHost_.Visibility(mux::Visibility::Collapsed);
    middle.Children().Append(treeCard_);
    middle.Children().Append(treeSizer_->element());
    middle.Children().Append(playlistCard);
    middle.Children().Append(panelSizer_->element());
    middle.Children().Append(panelCard_);
    middle.Children().Append(toolsSizer_->element());
    middle.Children().Append(toolsHost_);
    syncSizers();

    // After the playlist, which the enabled states read.
    commands_ = std::make_unique<CommandMenus>(CommandMenus::Hooks{
        [this](app::CommandId id) { onCommand(id); },
        [this](app::CommandId id) { return enabled(id); },
        [this](app::CommandId id) { return checked(id); },
        &MainWindow::offered,
    });
    // The menu bar, in the title bar after the icon and the player's name.
    // The shortcuts do not live in it -- see CommandMenus -- so they work with
    // every menu closed. It sits left in a host that fitTitleBar() sizes to
    // the content column, which the template would otherwise centre it in.
    {
        auto menuBar = commands_->menuBar();
        menuBar.HorizontalAlignment(mux::HorizontalAlignment::Left);
        menuBar.VerticalAlignment(mux::VerticalAlignment::Center);
        titleContent_ = mux::Controls::Grid();
        titleContent_.HorizontalAlignment(mux::HorizontalAlignment::Left);
        titleContent_.Children().Append(menuBar);
        titleBar_.Content(titleContent_);
        titleBar_.SizeChanged([this](auto&&, auto&&) { fitTitleBar(); });
    }

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
    mux::Controls::Grid::SetRow(middle, 2);
    mux::Controls::Grid::SetRow(status_, 3);
    root.Children().Append(titleBar_);
    root.Children().Append(transport);
    root.Children().Append(middle);
    root.Children().Append(status_);
    // On the root, so the keys reach them from anywhere in the window.
    commands_->attachAccelerators(root);
    window_.Content(root);
    window_.SetTitleBar(titleBar_);

    restoreState();
}

namespace {

/// The descendant of `root` called `name`, depth first, or null. For parts of
/// a control's template, which are not reachable through the control's own
/// namescope from out here.
mux::FrameworkElement findNamed(const mux::DependencyObject& root, std::wstring_view name) {
    const int count = mux::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        const auto child = mux::Media::VisualTreeHelper::GetChild(root, i);
        if (const auto element = child.try_as<mux::FrameworkElement>();
            element && element.Name() == name)
            return element;
        if (auto found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

}  // namespace

void MainWindow::fitTitleBar() {
    // The TitleBar template puts its content in a presenter aligned by a theme
    // resource -- centred, for a search box -- and its compact state pins it
    // Left whatever the resource says (microsoft-ui-xaml #11181). The menu
    // bar belongs at the left in both, so its host is given the width of the
    // template's content column outright, which is what is left once the
    // icon, title, track and caption buttons have theirs.
    //
    // The column is deferred-load, realised when the content is first set,
    // so this looks it up until it has it.
    if (!titleContent_)
        return;
    const auto column = contentColumn_ ? contentColumn_
                                       : findNamed(titleBar_, L"PART_ContentPresenterGrid");
    if (!column)
        return;
    if (!contentColumn_) {
        contentColumn_ = column;
        column.SizeChanged([this](auto&&, auto&&) { fitTitleBar(); });
    }
    double inset = 0;
    if (auto presenter = findNamed(column, L"PART_ContentPresenter")) {
        const auto margin = presenter.Margin();
        inset += margin.Left + margin.Right;
    }
    const double width = column.ActualWidth() - inset;
    if (width > 0 && width != titleContent_.Width())
        titleContent_.Width(width);
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

    // The seek bar seeks on release, not on every step of a drag -- a seek is a
    // decoder reopen for some formats -- and while held, the clock shows where
    // the release would land.
    subscriptions_.push_back(seekBar_->seekRequested.connect(
        [this](double seconds) { session_.playback().seek(seconds); }));
    subscriptions_.push_back(
        seekBar_->scrubbed.connect([this](double seconds) { setClock(seconds, duration_); }));
    observe(session_.waveformUpdated, [this](const std::shared_ptr<const WaveformSummary>& summary) {
        seekBar_->setWaveform(summary);
    });
    applyWaveformSetting();

    window_.Closed([this](auto&&, auto&&) {
        persistState();
        // Preferences goes with the player: WinUI keeps the process running
        // for as long as any window is open, and an orphaned settings window
        // would be a player with no player.
        if (preferences_) {
            preferences_->close();
        }
        if (closed) {
            closed();
        }
    });

    // --- the commands' state ------------------------------------------------
    //
    // Everything an enabled or checked state reads from, so the shortcuts are
    // live the moment they become meaningful rather than when a menu is next
    // opened. See CommandMenus.
    playlist_->selectionChanged = [this] {
        refreshCommands();
        if (session_.settings().PanelFollowMode() == 0) {
            refreshPanels();
        }
    };

    // --- the panes ------------------------------------------------------------
    subscriptions_.push_back(equalizer_->settingChanged.connect(
        [this](const std::string& key) { session_.settingChanged(key); }));
    subscriptions_.push_back(speed_->settingChanged.connect(
        [this](const std::string& key) { session_.settingChanged(key); }));
    for (auto* signal : {&spectrum_->settingChanged, &scope_->settingChanged}) {
        subscriptions_.push_back(
            signal->connect([this](const std::string& key) { session_.settingChanged(key); }));
    }
    // The panes' own Preferences..., each to its page.
    for (auto* signal : {&spectrum_->settingsRequested, &scope_->settingsRequested}) {
        subscriptions_.push_back(
            signal->connect([this] { showPreferences(PreferencesPage::Visualizers); }));
    }
    subscriptions_.push_back(
        speed_->settingsRequested.connect([this] { showPreferences(PreferencesPage::PitchTempo); }));
    // What the session changes on its own -- a genre's preset at a track
    // boundary, a setting from the remote control -- reaches the panes here.
    observe(session_.effectApplied, [this](app::Effect effect, const std::string&) {
        switch (effect) {
            case app::Effect::EqualizerCurve:
                equalizer_->refresh();
                break;
            case app::Effect::RefreshSpeed:
                speed_->refresh();
                break;
            case app::Effect::RefreshSpectrum:
                spectrum_->applySettings(session_.settings());
                break;
            case app::Effect::RefreshScope:
                scope_->applySettings(session_.settings());
                break;
            case app::Effect::WaveformSeekBar:
                applyWaveformSetting();
                break;
            case app::Effect::RefreshPanels:
                lyrics_->setTimed(session_.settings().LyricsSynced());
                refreshPanels();
                break;
            case app::Effect::OnlineLyrics:
                // LRCLIB switched on or off, or pointed elsewhere: the session
                // hands out a lookup only while it is on, so the pane is given
                // the one it now has -- the one it was built with stays null
                // after a launch with LRCLIB off, and nothing would be asked.
                lyrics_->setLookup(session_.lyricsLookup());
                refreshPanels();
                break;
            default:
                break;
        }
        refreshCommands();
    });
    const auto add = [this](const std::vector<Url>& urls) { session_.addUrls(urls); };
    subscriptions_.push_back(fileTree_->activated.connect(add));
    subscriptions_.push_back(fileTree_->addRequested.connect(add));
    subscriptions_.push_back(fileTree_->rootChosen.connect([this] {
        session_.settings().setRawValue("xpcog.fileTree.root", fileTree_->rootPath());
    }));

    // The size and place to come back to: AppWindow reports the maximised
    // bounds while maximised, and those are not what a restore should use.
    window_.AppWindow().Changed([this](winrt::Microsoft::UI::Windowing::AppWindow const& window, auto&&) {
        const auto presenter = window.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
        if (presenter && presenter.State() == winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Restored) {
            normalBounds_ = {window.Position().X, window.Position().Y, window.Size().Width,
                             window.Size().Height};
        }
    });
    playlist_->contextMenuRequested =
        [this](const mux::UIElement& target, std::optional<winrt::Windows::Foundation::Point> at) {
            showPlaylistMenu(target, at);
        };
    subscriptions_.push_back(session_.view().rebuilt.connect([this] { refreshCommands(); }));
    observe(session_.tracksUpdated, [this] { refreshCommands(); });
    observe(session_.scanFinished, [this] { refreshCommands(); });

    // Which shortcuts a text box should have to itself: see setTextFocus().
    window_.Content().as<mux::UIElement>().GotFocus([this](auto&&, mux::RoutedEventArgs const& args) {
        const auto source = args.OriginalSource();
        commands_->setTextFocus(source.try_as<mux::Controls::TextBox>() != nullptr ||
                                source.try_as<mux::Controls::AutoSuggestBox>() != nullptr);
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
    // And at the title bar's right end, with the whole of it as its tooltip
    // for when it is trimmed.
    trackText_.Text(toH(text));
    mux::Controls::ToolTipService::SetToolTip(
        trackText_, text.empty() ? nullptr : winrt::box_value(toH(text)));
    fitTitleBar();
    refreshCommands();
    refreshPanels();
}

void MainWindow::onPlaybackStateChanged(bool playing, bool paused) {
    // The band table is built against the device's rate, which is known
    // only once a track has opened one.
    spectrum_->setSampleRate(session_.playback().sampleRate());
    scope_->setSampleRate(session_.playback().sampleRate());
    refreshVisualizers();
    const bool showsPause = playing && !paused;
    playGlyph_.Glyph(showsPause ? kGlyphPause : kGlyphPlay);
    const winrt::hstring label = toH(app::tr(showsPause ? "Pause" : "Play"));
    mux::Controls::ToolTipService::SetToolTip(playButton_, winrt::box_value(label));
    mux::Automation::AutomationProperties::SetName(playButton_, label);
    if (!playing) {
        onPositionChanged(0, 0);
    }
    refreshCommands();
    refreshPanels();
}

void MainWindow::onPositionChanged(double seconds, double duration) {
    duration_ = duration;
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);
    if (!seekBar_->scrubbing()) {
        setClock(seconds, duration);
    }
}

void MainWindow::applyWaveformSetting() {
    const Settings& settings = session_.settings();
    seekBar_->setWaveformStyle(SeekBar::styleFrom(settings));
    seekBar_->setWaveformMode(settings.WaveformSeekBar());
}

void MainWindow::setClock(double seconds, double duration) {
    clock_.Text(toH(app::formatClock(seconds) + " / " + app::formatClock(duration)));
}

void MainWindow::setStatus(const std::string& text) {
    status_.Text(toH(text));
}

// --- commands ---------------------------------------------------------------------

std::vector<TrackId> MainWindow::selectedTracks() const {
    std::vector<TrackId> ids;
    for (const std::size_t row : playlist_->selectedRows()) {
        if (const TrackId id = session_.view().trackAt(row); id != kInvalidTrackId) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool MainWindow::selectionHasFiles() const {
    for (const TrackId id : selectedTracks()) {
        const PlaylistEntry* entry = session_.playlist().find(id);
        if (entry != nullptr && entry->url.localPath().has_value()) {
            return true;
        }
    }
    return false;
}

bool MainWindow::offered(app::CommandId id) {
    using app::CommandId;
    switch (id) {
        // No docking, so nothing ever floats to be docked -- the GTK player's
        // decision too, and for good: there is no dock manager to come back.
        case CommandId::ViewDockPanes:
        // No MIDI build, no panel to show. And the mini player is a later step
        // of the port: a menu item that does nothing is worse than one that is
        // not there.
#ifndef XPCOG_HAVE_SC55_PANEL
        case CommandId::ViewSc55Panel:
#endif
        case CommandId::ViewMiniPlayer:
            return false;
        default:
            return true;
    }
}

bool MainWindow::enabled(app::CommandId id) const {
    using app::CommandId;
    // The same rules as the GTK window's refreshActionState(), so the players
    // agree about when a command means something.
    const bool anything = !session_.playlist().empty();
    switch (id) {
        case CommandId::EditUndo:
            return session_.undo().canUndo();
        case CommandId::EditRedo:
            return session_.undo().canRedo();
        case CommandId::PlaybackPlayPause:
        case CommandId::PlaybackNext:
        case CommandId::PlaybackPrevious:
        case CommandId::EditRandomize:
        case CommandId::EditSelectAll:
            return anything;
        case CommandId::PlaybackStop:
            return session_.playback().playing();
        case CommandId::EditScrollToCurrent:
            return session_.currentTrack() != kInvalidTrackId;
        case CommandId::EditRemove:
        case CommandId::PlaybackEnqueue:
        case CommandId::PlaylistToggleQueued:
        case CommandId::PlaylistStopAfter:
        case CommandId::PlaylistSaveSelection:
        case CommandId::PlaylistSearchArtist:
        case CommandId::PlaylistSearchAlbum:
        case CommandId::PlaylistReloadInfo:
        case CommandId::PlaylistResetPlayCount:
            return playlist_->hasSelection();
        case CommandId::PlaylistRemoveRating:
            return playlist_->hasSelection() && session_.library() != nullptr;
        case CommandId::PlaylistReveal:
        case CommandId::PlaylistTrash:
            return selectionHasFiles();
        default:
            return true;
    }
}

std::optional<bool> MainWindow::checked(app::CommandId id) const {
    using app::CommandId;
    const Settings& settings = session_.settings();
    const int       value    = static_cast<int>(id);
    if (id >= CommandId::OrderRepeatNone && id <= CommandId::OrderRepeatAll) {
        return std::clamp(settings.RepeatMode(), 0, 3) ==
               value - static_cast<int>(CommandId::OrderRepeatNone);
    }
    if (id >= CommandId::OrderShuffleOff && id <= CommandId::OrderShuffleAll) {
        return std::clamp(settings.ShuffleMode(), 0, 2) ==
               value - static_cast<int>(CommandId::OrderShuffleOff);
    }
    switch (id) {
        case CommandId::ViewFileTree:
            return fileTreeShown();
        case CommandId::ViewInfo:
            return panelShown() && panelPage_ == "info";
        case CommandId::ViewLyrics:
            return panelShown() && panelPage_ == "lyrics";
        case CommandId::ViewTimedLyrics:
            return settings.LyricsSynced();
        case CommandId::ViewFollowSelection:
            return settings.PanelFollowMode() == 0;
        case CommandId::ViewFollowPlayback:
            return settings.PanelFollowMode() == 1;
        case CommandId::ViewSpectrum:
            return tools_ && tools_->shown("spectrum");
        case CommandId::ViewOscilloscope:
            return tools_ && tools_->shown("scope");
        case CommandId::ViewWaveform:
            return settings.WaveformSeekBar();
        case CommandId::ViewSc55Panel:
            return tools_ && tools_->shown("sc55");
        case CommandId::ViewEqualizer:
            return tools_ && tools_->shown("equalizer");
        case CommandId::ViewSpeed:
            return tools_ && tools_->shown("speed");
        default:
            break;
    }
    return std::nullopt;
}

void MainWindow::refreshCommands() {
    if (commands_) {
        commands_->refresh();
    }
}

void MainWindow::onCommand(app::CommandId id) {
    using app::CommandId;
    Settings& settings = session_.settings();
    switch (id) {
        case CommandId::FileOpen:
            openFiles();
            break;
        case CommandId::FileOpenFolder:
            openFolder();
            break;
        case CommandId::FileOpenUrl:
            openUrl();
            break;
        case CommandId::FileSavePlaylist:
            savePlaylist(false);
            break;
        case CommandId::PlaylistSaveSelection:
            savePlaylist(true);
            break;
        case CommandId::FileQuit:
            window_.Close();
            break;
        case CommandId::FilePreferences:
            showPreferences(std::nullopt);
            break;
        case CommandId::HelpAbout:
            showAbout();
            break;

        case CommandId::PlaybackPlayPause:
            session_.playback().playPause();
            break;
        case CommandId::PlaybackStop:
            session_.playback().stop();
            break;
        case CommandId::PlaybackNext:
            session_.playback().next();
            break;
        case CommandId::PlaybackPrevious:
            session_.playback().previous();
            break;

        case CommandId::EditUndo:
            session_.undo().undo();
            break;
        case CommandId::EditRedo:
            session_.undo().redo();
            break;
        case CommandId::EditSelectAll:
            playlist_->selectAll();
            break;
        case CommandId::EditScrollToCurrent:
            // Cog's "select currently playing": scroll to it and select it,
            // replacing the selection. A filter can be hiding it, and saying
            // so beats doing nothing.
            if (const auto row = session_.view().rowForTrack(session_.currentTrack())) {
                playlist_->selectOnly(*row);
            } else {
                setStatus(app::tr("The playing track is hidden by the filter"));
            }
            break;
        case CommandId::EditRemove:
            if (session_.commands().remove(selectedTracks()) > 0) {
                setStatus(session_.statusSummary());
            }
            break;
        case CommandId::EditRandomize:
            session_.commands().randomize();
            break;

        case CommandId::PlaybackEnqueue:
            session_.commands().setQueued(selectedTracks(), true);
            break;
        case CommandId::PlaylistToggleQueued:
            session_.commands().toggleQueued(selectedTracks());
            break;
        case CommandId::PlaylistStopAfter:
            session_.commands().toggleStopAfter(selectedTracks());
            break;
        case CommandId::PlaylistSearchArtist:
        case CommandId::PlaylistSearchAlbum: {
            // Into the filter, which is what searching the playlist is here;
            // the first selected row decides what to look for.
            const std::vector<TrackId> selected = selectedTracks();
            const PlaylistEntry*       entry =
                selected.empty() ? nullptr : session_.playlist().find(selected.front());
            if (entry != nullptr) {
                playlist_->setFilter(id == CommandId::PlaylistSearchAlbum ? entry->album.str()
                                                                           : entry->artist.str());
            }
            break;
        }
        case CommandId::PlaylistReloadInfo:
            session_.reloadTracks(selectedTracks());
            break;
        case CommandId::PlaylistResetPlayCount:
            session_.resetPlayCount(selectedTracks());
            break;
        case CommandId::PlaylistRemoveRating:
            session_.removeRating(selectedTracks());
            break;
        case CommandId::PlaylistReveal:
            session_.revealInFileManager(selectedTracks());
            break;
        case CommandId::PlaylistTrash:
            trashSelected();
            break;

        case CommandId::OrderRepeatNone:
        case CommandId::OrderRepeatOne:
        case CommandId::OrderRepeatAlbum:
        case CommandId::OrderRepeatAll:
            settings.setRepeatMode(static_cast<int>(id) -
                                   static_cast<int>(CommandId::OrderRepeatNone));
            session_.settingChanged("repeat");
            break;
        case CommandId::OrderShuffleOff:
        case CommandId::OrderShuffleAlbums:
        case CommandId::OrderShuffleAll:
            settings.setShuffleMode(static_cast<int>(id) -
                                    static_cast<int>(CommandId::OrderShuffleOff));
            session_.settingChanged("shuffle");
            break;

        case CommandId::ViewFileTree:
            showFileTree(!fileTreeShown());
            break;
        case CommandId::ViewFileTreeRoot:
            fileTree_->chooseRootPath();
            break;
        case CommandId::ViewInfo:
            togglePanel("info");
            break;
        case CommandId::ViewLyrics:
            togglePanel("lyrics");
            break;
        case CommandId::ViewTimedLyrics:
            settings.setLyricsSynced(!settings.LyricsSynced());
            session_.settingChanged("lyricsSynced");
            lyrics_->setTimed(settings.LyricsSynced());
            refreshPanels();
            break;
        case CommandId::ViewFollowSelection:
        case CommandId::ViewFollowPlayback:
            settings.setPanelFollowMode(id == CommandId::ViewFollowSelection ? 0 : 1);
            session_.settingChanged("panelFollowMode");
            refreshPanels();
            break;
        case CommandId::ViewSpectrum:
            showTool("spectrum", !tools_->shown("spectrum"));
            break;
        case CommandId::ViewOscilloscope:
            showTool("scope", !tools_->shown("scope"));
            break;
        case CommandId::ViewWaveform:
            settings.setWaveformSeekBar(!settings.WaveformSeekBar());
            session_.settingChanged("waveformSeekBar");
            applyWaveformSetting();
            break;
        case CommandId::ViewSc55Panel:
            showTool("sc55", !tools_->shown("sc55"));
            break;
        case CommandId::ViewEqualizer:
            showTool("equalizer", !tools_->shown("equalizer"));
            break;
        case CommandId::ViewSpeed:
            showTool("speed", !tools_->shown("speed"));
            break;

        default:
            break;
    }
}

void MainWindow::showPlaylistMenu(const mux::UIElement& target,
                                  std::optional<winrt::Windows::Foundation::Point> at) {
    refreshCommands();
    auto menu    = commands_->playlistMenu();
    auto options = mux::Controls::Primitives::FlyoutShowOptions();
    if (at) {
        options.Position(*at);
    }
    menu.ShowAt(target, options);
}

// --- the panes ----------------------------------------------------------------------

bool MainWindow::fileTreeShown() const {
    return treeCard_.Visibility() == mux::Visibility::Visible;
}

void MainWindow::showFileTree(bool show) {
    treeCard_.Visibility(show ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    syncSizers();
    refreshCommands();
}

void MainWindow::syncSizers() {
    const auto shown = [](bool on) { return on ? mux::Visibility::Visible : mux::Visibility::Collapsed; };
    treeSizer_->element().Visibility(shown(fileTreeShown()));
    panelSizer_->element().Visibility(shown(panelShown()));
    toolsSizer_->element().Visibility(shown(tools_->anyShown()));
}

bool MainWindow::panelShown() const {
    return panelCard_.Visibility() == mux::Visibility::Visible;
}

void MainWindow::showPanelPage(const std::string& page) {
    panelPage_ = page;
    info_->element().Visibility(page == "info" ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    lyrics_->element().Visibility(page == "lyrics" ? mux::Visibility::Visible
                                                   : mux::Visibility::Collapsed);
    // The selector follows when the page was chosen from the menu. Setting
    // the item it already has raises nothing, so this does not come back here.
    for (const auto& item : panelSelector_.Items()) {
        if (toUtf8(winrt::unbox_value<winrt::hstring>(item.Tag())) == page) {
            panelSelector_.SelectedItem(item);
        }
    }
    refreshPanels();
    refreshCommands();
}

void MainWindow::togglePanel(const std::string& page) {
    if (panelShown() && panelPage_ == page) {
        panelCard_.Visibility(mux::Visibility::Collapsed);
    } else {
        panelCard_.Visibility(mux::Visibility::Visible);
        showPanelPage(page);
    }
    syncSizers();
    refreshCommands();
}

void MainWindow::refreshVisualizers() {
    // Running only while shown and something is audible, as GTK's do.
    const bool playing = session_.playback().playing() && !session_.playback().paused();
    spectrum_->setActive(tools_->shown("spectrum") && playing);
    scope_->setActive(tools_->shown("scope") && playing);
#ifdef XPCOG_HAVE_SC55_PANEL
    // Whether or not anything plays: the panel shows the synth's idle state
    // too, as GTK's does.
    sc55_->setActive(tools_->shown("sc55"));
#endif
}

void MainWindow::showTool(const std::string& name, bool show) {
    tools_->setShown(name, show);
    toolsHost_.Visibility(tools_->anyShown() ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    refreshVisualizers();
    syncSizers();
    refreshCommands();
}

TrackId MainWindow::panelTrackId() const {
    if (session_.settings().PanelFollowMode() == 1) {
        return session_.currentTrack();
    }
    const std::vector<TrackId> selection = selectedTracks();
    return selection.empty() ? session_.currentTrack() : selection.front();
}

void MainWindow::refreshPanels() {
    // A hidden panel is not kept up to date: the lyrics page would look the
    // track up on the network for nobody. Showing it refreshes it.
    if (!panelCard_ || !panelShown()) {
        return;
    }
    const TrackId        id    = panelTrackId();
    const PlaylistEntry* entry = session_.playlist().find(id);
    if (panelPage_ == "lyrics") {
        lyrics_->showEntry(entry, id != kInvalidTrackId && id == session_.currentTrack() &&
                                      session_.playback().playing());
    } else {
        info_->showEntry(entry);
    }
}

// --- what is remembered -------------------------------------------------------------
//
// The file tree's root and whether it is shown are the keys the wx and GTK
// players use, so a library folder chosen in one is the one the others open
// at. The window's size and the panes are this player's own: its layout is
// not either of theirs.

namespace {

constexpr std::string_view kPlacementKey = "xpcog.winui.window.placement";
constexpr std::string_view kPanesKey     = "xpcog.winui.window.panes";

std::string musicFolder() {
    PWSTR path = nullptr;
    std::string found;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path))) {
        found = winrt::to_string(path);
    }
    ::CoTaskMemFree(path);
    return found;
}

}  // namespace

void MainWindow::persistState() {
    Settings& settings = session_.settings();

    const auto window    = window_.AppWindow();
    const auto presenter = window.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
    const bool maximised = presenter && presenter.State() ==
                                            winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Maximized;
    if (normalBounds_.Width > 0 && normalBounds_.Height > 0) {
        char text[96];
        std::snprintf(text, sizeof text, "%d,%d,%d,%d,%d", normalBounds_.X, normalBounds_.Y,
                      normalBounds_.Width, normalBounds_.Height, maximised ? 1 : 0);
        settings.setRawValue(kPlacementKey, text);
    }

    // Not the tree's root: that is saved when it is chosen (rootChosen, in
    // wireUp) and never here. A root the tree could not open -- a share that
    // is offline -- leaves rootPath() empty, and writing that back at close
    // wiped the remembered root for all three players.
    settings.setRawValue("xpcog.window.fileTree", fileTreeShown() ? "1" : "0");

    std::string panes;
    for (const char* name : {"spectrum", "scope", "equalizer", "speed", "sc55"}) {
        panes += std::string(name) + "=" + (tools_->shown(name) ? "1" : "0") + ";";
    }
    panes += std::string("panels=") + (panelShown() ? "1" : "0") + ";";
    panes += "page=" + panelPage_ + ";";
    // The sizes the sizers left, in DIPs. The strip's only once it has been
    // dragged: until then it is as tall as its contents, which is not a size.
    panes += "tree=" + std::to_string(std::lround(treeCard_.Width())) + ";";
    panes += "panel=" + std::to_string(std::lround(panelCard_.Width())) + ";";
    if (const double strip = toolsHost_.Height(); !std::isnan(strip)) {
        panes += "strip=" + std::to_string(std::lround(strip)) + ";";
    }
    settings.setRawValue(kPanesKey, panes);
}

void MainWindow::restoreState() {
    Settings& settings = session_.settings();
    const auto window  = window_.AppWindow();

    // Size and place, in physical pixels -- but only onto a display that is
    // still there. A window saved on a monitor since unplugged would open
    // nowhere anyone can see it; that one gets the default size instead.
    const double scale = ::GetDpiForWindow(hwnd()) / 96.0;
    winrt::Windows::Graphics::RectInt32 bounds{0, 0, static_cast<int32_t>(1280 * scale),
                                               static_cast<int32_t>(820 * scale)};
    int x = 0, y = 0, width = 0, height = 0, maximised = 0;
    const std::string placement = settings.rawValue(kPlacementKey);
    if (std::sscanf(placement.c_str(), "%d,%d,%d,%d,%d", &x, &y, &width, &height, &maximised) == 5 &&
        width > 0 && height > 0 &&
        winrt::Microsoft::UI::Windowing::DisplayArea::GetFromRect(
            {x, y, width, height}, winrt::Microsoft::UI::Windowing::DisplayAreaFallback::None)) {
        bounds = {x, y, width, height};
        window.MoveAndResize(bounds);
        maximizeOnShow_ = maximised != 0;
    } else {
        window.Resize({bounds.Width, bounds.Height});
        bounds = {window.Position().X, window.Position().Y, bounds.Width, bounds.Height};
    }
    normalBounds_ = bounds;

    // The Music folder only when nothing was ever chosen. A saved root that
    // cannot be reached now -- the share is offline -- leaves the tree empty
    // instead: showing Music would look like the choice had been forgotten.
    const std::string root = settings.rawValue("xpcog.fileTree.root");
    fileTree_->setRootPath(root.empty() ? musicFolder() : root);
    showFileTree(settings.rawValue("xpcog.window.fileTree") == "1");

    const std::string panes = settings.rawValue(kPanesKey);
    std::string_view  rest  = panes;
    bool              panels = false;
    while (!rest.empty()) {
        const std::size_t      semicolon = rest.find(';');
        const std::string_view entry     = rest.substr(0, semicolon);
        rest = semicolon == std::string_view::npos ? std::string_view{} : rest.substr(semicolon + 1);
        const std::size_t equals = entry.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string key(entry.substr(0, equals));
        const std::string value(entry.substr(equals + 1));
        const auto number = [&value](double low, double high) -> std::optional<double> {
            double parsed = 0;
            if (std::sscanf(value.c_str(), "%lf", &parsed) != 1 || parsed < low || parsed > high) {
                return std::nullopt;  // hand-edited, or from a larger screen
            }
            return parsed;
        };
        if (key == "panels") {
            panels = value == "1";
        } else if (key == "tree") {
            if (const auto width = number(kMinTreeWidth, kMaxTreeWidth)) {
                treeCard_.Width(*width);
            }
        } else if (key == "panel") {
            if (const auto width = number(kMinPanelWidth, kMaxPanelWidth)) {
                panelCard_.Width(*width);
            }
        } else if (key == "strip") {
            if (const auto height = number(kMinStripHeight, 4000)) {
                toolsHost_.Height(*height);
            }
        } else if (key == "page" && (value == "info" || value == "lyrics")) {
            showPanelPage(value);
        } else if (key == "spectrum" || key == "scope" || key == "equalizer" || key == "speed" ||
                   key == "sc55") {
            showTool(key, value == "1");
        }
    }
    panelCard_.Visibility(panels ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    syncSizers();
    refreshPanels();
}

// --- dialogs ----------------------------------------------------------------------

void MainWindow::showPreferences(std::optional<PreferencesPage> page) {
    // One window, brought forward and turned to the page if already open.
    if (!preferences_) {
        preferences_ = std::make_unique<PreferencesWindow>(session_, hwnd());
        preferencesSubscriptions_.push_back(preferences_->settingChanged.connect(
            [this](const std::string& key) { session_.settingChanged(key); }));
        // Destroyed after its Closed event has finished, not inside it: the
        // window is still in the middle of closing when it says so.
        preferencesSubscriptions_.push_back(preferences_->closed.connect([this] {
            session_.dispatcher()([this] {
                preferencesSubscriptions_.clear();
                preferences_.reset();
            });
        }));
    }
    preferences_->show(page);
}

mux::Controls::ContentDialog MainWindow::dialog(const std::string& title) const {
    auto box = mux::Controls::ContentDialog();
    // A ContentDialog is not a window: it draws in this one's XAML tree, and
    // has to be told which.
    box.XamlRoot(window_.Content().XamlRoot());
    // WinUI 3 does not apply the Fluent dialog style by itself; without this
    // the dialog comes up in the old, square, pre-Windows 11 template.
    box.Style(mux::Application::Current()
                  .Resources()
                  .Lookup(winrt::box_value(L"DefaultContentDialogStyle"))
                  .as<mux::Style>());
    box.Title(winrt::box_value(toH(title)));
    box.DefaultButton(mux::Controls::ContentDialogButton::Primary);
    return box;
}

namespace pickers = winrt::Microsoft::Windows::Storage::Pickers;

winrt::fire_and_forget MainWindow::openFiles() {
    try {
        pickers::FileOpenPicker picker(window_.AppWindow().Id());
        auto audio = winrt::single_threaded_vector<winrt::hstring>();
        for (const std::string& extension : session_.registry().allExtensions()) {
            audio.Append(toH("." + extension));
        }
        picker.FileTypeChoices().Insert(toH(app::tr("Audio Files")), audio);
        picker.FileTypeChoices().Insert(toH(app::tr("All Files")),
                                        winrt::single_threaded_vector<winrt::hstring>({L"*"}));

        const auto chosen = co_await picker.PickMultipleFilesAsync();
        std::vector<Url> urls;
        for (const auto& file : chosen) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path(std::wstring(file.Path()))));
        }
        if (!urls.empty()) {
            session_.addUrls(urls);
        }
    } catch (const winrt::hresult_error& error) {
        setStatus(toUtf8(error.message()));
    }
}

winrt::fire_and_forget MainWindow::openFolder() {
    try {
        pickers::FolderPicker picker(window_.AppWindow().Id());
        const auto chosen = co_await picker.PickSingleFolderAsync();
        if (chosen) {
            session_.addUrls({Url::fromLocalPath(std::filesystem::path(std::wstring(chosen.Path())))});
        }
    } catch (const winrt::hresult_error& error) {
        setStatus(toUtf8(error.message()));
    }
}

winrt::fire_and_forget MainWindow::savePlaylist(bool selectionOnly) {
    // No point asking for a name for an empty selection.
    std::vector<TrackId> selection;
    if (selectionOnly) {
        selection = selectedTracks();
        if (selection.empty()) {
            co_return;
        }
    }
    try {
        pickers::FileSavePicker picker(window_.AppWindow().Id());
        // A different name for a selection, as the wx frame does, so two saves
        // in a row do not offer to overwrite each other.
        picker.SuggestedFileName(selectionOnly ? L"selection" : L"playlist");
        picker.FileTypeChoices().Insert(toH(app::tr("M3U Playlist (*.m3u8)")),
                                        winrt::single_threaded_vector<winrt::hstring>({L".m3u8"}));
        picker.FileTypeChoices().Insert(toH(app::tr("PLS Playlist (*.pls)")),
                                        winrt::single_threaded_vector<winrt::hstring>({L".pls"}));
        picker.FileTypeChoices().Insert(toH(app::tr("XSPF Playlist (*.xspf)")),
                                        winrt::single_threaded_vector<winrt::hstring>({L".xspf"}));

        const auto chosen = co_await picker.PickSaveFileAsync();
        if (!chosen) {
            co_return;
        }
        const std::filesystem::path path(std::wstring(chosen.Path()));
        if (!session_.savePlaylist(path, selectionOnly ? &selection : nullptr)) {
            setStatus(app::tr("Could not write the playlist."));
        }
    } catch (const winrt::hresult_error& error) {
        setStatus(toUtf8(error.message()));
    }
}

winrt::fire_and_forget MainWindow::openUrl() {
    if (dialogOpen_) {
        co_return;
    }
    dialogOpen_ = true;

    auto box   = dialog(app::tr("Open URL"));
    auto entry = mux::Controls::TextBox();
    entry.PlaceholderText(L"https://");
    entry.MinWidth(420);
    box.Content(entry);
    box.PrimaryButtonText(toH(app::tr("Open")));
    box.CloseButtonText(toH(app::tr("Cancel")));
    // Open only for something that parses, as the wx dialog's validator has it.
    box.IsPrimaryButtonEnabled(false);
    entry.TextChanged([box, entry](auto&&, auto&&) {
        box.IsPrimaryButtonEnabled(Url::parse(toUtf8(entry.Text())).has_value());
    });

    const auto result = co_await box.ShowAsync();
    dialogOpen_ = false;
    if (result == mux::Controls::ContentDialogResult::Primary) {
        if (const std::optional<Url> url = Url::parse(toUtf8(entry.Text()))) {
            session_.addUrls({*url});
        }
    }
}

winrt::fire_and_forget MainWindow::trashSelected() {
    std::vector<TrackId> ids;
    for (const TrackId id : selectedTracks()) {
        const PlaylistEntry* entry = session_.playlist().find(id);
        if (entry != nullptr && entry->url.localPath().has_value()) {
            ids.push_back(id);
        }
    }
    if (ids.empty() || dialogOpen_) {
        co_return;
    }

    // Asked once, with the wx frame's wording, until the listener says not to.
    if (!session_.settings().TrashAskedConsent()) {
        dialogOpen_ = true;
        std::string title = app::trn("Move %u file to the trash?", "Move %u files to the trash?",
                                     ids.size());
        if (const std::size_t at = title.find("%u"); at != std::string::npos) {
            title.replace(at, 2, std::to_string(ids.size()));
        }
        auto box  = dialog(title);
        auto body = mux::Controls::StackPanel();
        body.Spacing(12);
        auto text = mux::Controls::TextBlock();
        text.TextWrapping(mux::TextWrapping::Wrap);
        text.Text(toH(app::tr("Undo puts the rows back in the playlist. It does not bring the "
                              "files back -- restore those from the trash itself.")));
        auto again = mux::Controls::CheckBox();
        again.Content(winrt::box_value(toH(app::tr("Do not ask again"))));
        body.Children().Append(text);
        body.Children().Append(again);
        box.Content(body);
        box.PrimaryButtonText(toH(app::commandLabel(app::CommandId::PlaylistTrash)));
        box.CloseButtonText(toH(app::tr("Cancel")));
        // The destructive answer is not the default one.
        box.DefaultButton(mux::Controls::ContentDialogButton::Close);

        const auto result = co_await box.ShowAsync();
        dialogOpen_ = false;
        if (result != mux::Controls::ContentDialogResult::Primary) {
            co_return;
        }
        if (again.IsChecked().Value()) {
            session_.settings().setTrashAskedConsent(true);
        }
    }
    session_.trashTracks(ids);
}

winrt::fire_and_forget MainWindow::showAbout() {
    if (dialogOpen_) {
        co_return;
    }
    dialogOpen_ = true;
    auto box  = dialog("XPCog");
    auto body = mux::Controls::StackPanel();
    body.Spacing(4);
    auto version = mux::Controls::TextBlock();
    version.Text(toH(std::string(kVersionString)));
    // The credits and licences table is the wx About box's, and comes over
    // with the rest of that dialog; this says which build is running.
    auto note = mux::Controls::TextBlock();
    note.TextWrapping(mux::TextWrapping::Wrap);
    note.Text(L"The WinUI player is a preview.");
    body.Children().Append(version);
    body.Children().Append(note);
    box.Content(body);
    box.CloseButtonText(L"OK");
    box.DefaultButton(mux::Controls::ContentDialogButton::Close);
    co_await box.ShowAsync();
    dialogOpen_ = false;
}

}  // namespace xpcog::winui
