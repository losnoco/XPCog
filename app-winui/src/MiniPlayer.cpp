#include "MiniPlayer.hpp"

#include "Chrome.hpp"

#include "PlaybackController.hpp"
#include "Session.hpp"
#include "TrackText.hpp"
#include "Translations.hpp"

#include "xpcog/core/Settings.hpp"

#include <commctrl.h>
#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <cmath>

namespace xpcog::winui {
namespace {

namespace windowing = winrt::Microsoft::UI::Windowing;

/// The title bar's height with TitleBarHeightOption::Tall, in DIPs: the
/// window's whole height, since the title bar is all there is.
constexpr double kHeight = 48;
/// Wide enough for the controls and a usable seek bar; the starting width.
constexpr double kMinWidth     = 520;
constexpr double kInitialWidth = 640;
/// A waveform taller than this would not fit in the bar.
constexpr int kMaxWaveformHeight = 36;
constexpr UINT_PTR kSubclassId = 1;

}  // namespace

MiniPlayer::MiniPlayer(app::Session& session) : session_(session) {
    window_ = mux::Window();
    window_.as<IWindowNative>()->get_WindowHandle(&hwnd_);
    window_.Title(L"XPCog");
    window_.SystemBackdrop(mux::Media::MicaBackdrop());
    window_.ExtendsContentIntoTitleBar(true);
    window_.AppWindow().TitleBar().PreferredHeightOption(windowing::TitleBarHeightOption::Tall);
    window_.AppWindow().SetIcon(besideExecutable(L"xpcog.ico").wstring());

    auto titleBar = mux::Controls::TitleBar();
    {
        auto source = mux::Controls::ImageIconSource();
        source.ImageSource(mux::Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(
            L"file:///" + besideExecutable(L"xpcog.ico").generic_wstring())));
        titleBar.IconSource(source);
    }

    // The transport, as the main window's row has it, in the title bar.
    auto transport = mux::Controls::Grid();
    transport.ColumnSpacing(6);
    transport.VerticalAlignment(mux::VerticalAlignment::Center);
    transport.HorizontalAlignment(mux::HorizontalAlignment::Left);
    transport.Margin(mux::ThicknessHelper::FromLengths(4, 0, 8, 0));
    for (const auto& width : {mux::GridLengthHelper::Auto(),
                              mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star),
                              mux::GridLengthHelper::Auto(), mux::GridLengthHelper::Auto()}) {
        auto column = mux::Controls::ColumnDefinition();
        column.Width(width);
        transport.ColumnDefinitions().Append(column);
    }

    auto buttons = mux::Controls::StackPanel();
    buttons.Orientation(mux::Controls::Orientation::Horizontal);
    buttons.Spacing(2);
    buttons.Children().Append(glyphButton(kGlyphPrevious, app::tr("Previous"),
                                          [this] { session_.playback().previous(); }));
    playButton_ = glyphButton(kGlyphPlay, app::tr("Play"),
                              [this] { session_.playback().playPause(); });
    buttons.Children().Append(playButton_);
    buttons.Children().Append(glyphButton(kGlyphStop, app::tr("Stop"),
                                          [this] { session_.playback().stop(); }));
    buttons.Children().Append(glyphButton(kGlyphNext, app::tr("Next"),
                                          [this] { session_.playback().next(); }));
    transport.Children().Append(buttons);

    seekBar_ = std::make_unique<SeekBar>();
    {
        auto bar = seekBar_->element().as<mux::FrameworkElement>();
        bar.VerticalAlignment(mux::VerticalAlignment::Center);
        bar.Margin(mux::ThicknessHelper::FromLengths(4, 0, 4, 0));
        mux::Controls::Grid::SetColumn(bar, 1);
        transport.Children().Append(bar);
    }
    subscriptions_.push_back(seekBar_->seekRequested.connect(
        [this](double seconds) { session_.playback().seek(seconds); }));
    subscriptions_.push_back(seekBar_->scrubbed.connect(
        [this](double seconds) { clock_.Text(toH(app::formatClock(seconds))); }));
    applyWaveformSetting();

    // The position alone, as the wx MiniFrame's clock: there is not the room
    // for the length beside it, and the bar shows how far along it is.
    clock_ = mux::Controls::TextBlock();
    clock_.VerticalAlignment(mux::VerticalAlignment::Center);
    clock_.MinWidth(44);
    clock_.TextAlignment(mux::TextAlignment::Center);
    clock_.Text(L"0:00");
    mux::Controls::Grid::SetColumn(clock_, 2);
    transport.Children().Append(clock_);

    volume_ = mux::Controls::Slider();
    volume_.Minimum(0);
    volume_.Maximum(100);
    volume_.Width(96);
    volume_.VerticalAlignment(mux::VerticalAlignment::Center);
    volume_.Value(session_.playback().volume() * 100.0);
    mux::Controls::ToolTipService::SetToolTip(volume_, winrt::box_value(toH(app::tr("Volume"))));
    mux::Controls::Grid::SetColumn(volume_, 3);
    transport.Children().Append(volume_);
    volume_.ValueChanged([this](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        if (settingVolume_) {
            return;
        }
        const double gain = args.NewValue() / 100.0;
        session_.setVolume(gain);
        if (volumeChanged) {
            volumeChanged(gain);
        }
    });

    titleBar.Content(transport);
    fitTitleBarContent(titleBar, transport);

    auto root = mux::Controls::Grid();
    root.Children().Append(titleBar);
    window_.Content(root);
    window_.SetTitleBar(titleBar);

    // Resizable in width only -- the subclass below pins the height -- and so
    // not maximisable, which would only stretch one row across the screen.
    if (const auto presenter = window_.AppWindow().Presenter().try_as<windowing::OverlappedPresenter>()) {
        presenter.IsMaximizable(false);
    }
    ::SetWindowSubclass(hwnd(), &MiniPlayer::subclassProc, kSubclassId,
                        reinterpret_cast<DWORD_PTR>(this));
    const double scale = ::GetDpiForWindow(hwnd()) / 96.0;
    window_.AppWindow().ResizeClient(
        {static_cast<int32_t>(std::lround(kInitialWidth * scale)), clientHeight()});
    setFloating(session_.settings().FloatingMiniWindow());

    // A mode, not a window: closing means "go back to the full window", so
    // the close is refused and the owner told -- unless the player is quitting.
    window_.AppWindow().Closing([this](auto&&, windowing::AppWindowClosingEventArgs const& args) {
        if (closing_) {
            return;
        }
        args.Cancel(true);
        if (dismissed) {
            dismissed();
        }
    });
}

MiniPlayer::~MiniPlayer() {
    // The subclass first, so no message reaches a player that is going; by
    // handle, which stays a harmless value after the window has gone.
    ::RemoveWindowSubclass(hwnd_, &MiniPlayer::subclassProc, kSubclassId);
    close();
}

HWND MiniPlayer::hwnd() const {
    return hwnd_;
}

int MiniPlayer::clientHeight() const {
    return static_cast<int>(std::lround(kHeight * ::GetDpiForWindow(hwnd_) / 96.0));
}

LRESULT CALLBACK MiniPlayer::subclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR, DWORD_PTR self) {
    if (message == WM_GETMINMAXINFO) {
        // The height pinned to the title bar, and a floor under the width, in
        // physical pixels at this window's scale -- here rather than through
        // the presenter's preferred sizes, so the arithmetic is the window's
        // own: its frame is whatever the outer and client rectangles differ by.
        const auto* player = reinterpret_cast<const MiniPlayer*>(self);
        RECT outer{};
        RECT client{};
        ::GetWindowRect(hwnd, &outer);
        ::GetClientRect(hwnd, &client);
        const int frameX = (outer.right - outer.left) - (client.right - client.left);
        const int frameY = (outer.bottom - outer.top) - (client.bottom - client.top);
        const double scale  = ::GetDpiForWindow(hwnd) / 96.0;
        const int    height = player->clientHeight() + frameY;
        auto*        limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = static_cast<LONG>(std::lround(kMinWidth * scale)) + frameX;
        limits->ptMinTrackSize.y = height;
        limits->ptMaxTrackSize.y = height;
        return 0;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

void MiniPlayer::show() {
    setVolume(session_.playback().volume());
    window_.AppWindow().Show();
    window_.Activate();
    shown_ = true;
}

void MiniPlayer::hide() {
    window_.AppWindow().Hide();
    shown_ = false;
}

void MiniPlayer::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    shown_   = false;
    // After the loop has ended there is no window left to close, and a throw
    // from a destructor ends the process.
    try {
        window_.Close();
    } catch (const winrt::hresult_error&) {
    }
}

void MiniPlayer::setNowPlaying(const std::string& title, const std::string& artist) {
    if (title.empty()) {
        window_.Title(L"XPCog");
        return;
    }
    window_.Title(toH(artist.empty() ? title : title + " \xE2\x80\x94 " + artist));
}

void MiniPlayer::setPlaybackState(bool playing, bool paused) {
    const bool showsPause = playing && !paused;
    setGlyph(playButton_, showsPause ? kGlyphPause : kGlyphPlay,
             app::tr(showsPause ? "Pause" : "Play"));
    if (!playing) {
        setPosition(0, 0);
    }
}

void MiniPlayer::setPosition(double seconds, double duration) {
    duration_ = duration;
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);
    if (!seekBar_->scrubbing()) {
        clock_.Text(toH(app::formatClock(seconds)));
    }
}

void MiniPlayer::setVolume(double gain) {
    settingVolume_ = true;
    volume_.Value(gain * 100.0);
    settingVolume_ = false;
}

void MiniPlayer::setWaveform(std::shared_ptr<const WaveformSummary> summary) {
    seekBar_->setWaveform(std::move(summary));
}

void MiniPlayer::applyWaveformSetting() {
    const Settings& settings = session_.settings();
    SeekBar::WaveformStyle style = SeekBar::styleFrom(settings);
    style.height = std::min(style.height, kMaxWaveformHeight);
    seekBar_->setWaveformStyle(style);
    seekBar_->setWaveformMode(settings.WaveformSeekBar());
}

void MiniPlayer::setFloating(bool floating) {
    if (const auto presenter = window_.AppWindow().Presenter().try_as<windowing::OverlappedPresenter>()) {
        presenter.IsAlwaysOnTop(floating);
    }
}

}  // namespace xpcog::winui
