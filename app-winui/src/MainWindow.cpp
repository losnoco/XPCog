#include "MainWindow.hpp"

#include "Session.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/Version.hpp"

#include <winrt/Microsoft.Windows.Storage.Pickers.h>

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

    filter_ = mux::Controls::AutoSuggestBox();
    auto& filter = filter_;
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

    // After the playlist, which the enabled states read.
    commands_ = std::make_unique<CommandMenus>(CommandMenus::Hooks{
        [this](app::CommandId id) { onCommand(id); },
        [this](app::CommandId id) { return enabled(id); },
        [this](app::CommandId id) { return checked(id); },
        &MainWindow::offered,
    });
    auto menuBar = commands_->menuBar();
    menuBar.Margin(mux::ThicknessHelper::FromLengths(4, 0, 0, 0));

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
                               mux::GridLengthHelper::Auto(),
                               mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                               mux::GridLengthHelper::Auto()}) {
        auto row = mux::Controls::RowDefinition();
        row.Height(height);
        root.RowDefinitions().Append(row);
    }
    mux::Controls::Grid::SetRow(menuBar, 1);
    mux::Controls::Grid::SetRow(transport, 2);
    mux::Controls::Grid::SetRow(card, 3);
    mux::Controls::Grid::SetRow(status_, 4);
    root.Children().Append(titleBar_);
    root.Children().Append(menuBar);
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

    // --- the commands' state ------------------------------------------------
    //
    // Everything an enabled or checked state reads from, so the shortcuts are
    // live the moment they become meaningful rather than when a menu is next
    // opened. See CommandMenus.
    playlist_->selectionChanged = [this] { refreshCommands(); };
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
    titleBar_.Subtitle(toH(text));
    refreshCommands();
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
    refreshCommands();
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
        // The rest have nowhere to go yet: the panes, the preferences and the
        // mini player are later steps of the port, and a menu item that does
        // nothing is worse than one that is not there.
        case CommandId::ViewFileTree:
        case CommandId::ViewFileTreeRoot:
        case CommandId::ViewSpectrum:
        case CommandId::ViewOscilloscope:
        case CommandId::ViewWaveform:
        case CommandId::ViewEqualizer:
        case CommandId::ViewSpeed:
        case CommandId::ViewInfo:
        case CommandId::ViewLyrics:
        case CommandId::ViewTimedLyrics:
        case CommandId::ViewFollowSelection:
        case CommandId::ViewFollowPlayback:
        case CommandId::ViewSc55Panel:
        case CommandId::ViewMiniPlayer:
        case CommandId::FilePreferences:
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
                filter_.Text(toH(id == CommandId::PlaylistSearchAlbum ? entry->album.str()
                                                                       : entry->artist.str()));
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

// --- dialogs ----------------------------------------------------------------------

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
