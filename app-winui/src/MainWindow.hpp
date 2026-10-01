#pragma once

#include "PlaylistTable.hpp"
#include "WinRT.hpp"

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::app {
class Session;
}

namespace xpcog::winui {

/// The player's window: Mica under everything, the title bar drawn by WinUI's
/// TitleBar control with the filter in it, a transport row, the playlist on a
/// content layer, and a status line.
///
/// Layered the way the Windows 11 design guidance asks: the window's own
/// surfaces are transparent so the Mica backdrop shows through, and the one
/// region that holds content -- the playlist -- sits on LayerFillColorDefault,
/// the low-opacity content-layer brush, as a card.
class MainWindow {
public:
    explicit MainWindow(app::Session& session);
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void activate();

    [[nodiscard]] HWND hwnd() const;

    /// The window has closed. The session should be saved now: the dispatcher
    /// that runs everything is about to stop.
    std::function<void()> closed;

private:
    void build();
    void wireUp();

    [[nodiscard]] mux::Controls::Button transportButton(const wchar_t* glyph,
                                                       const std::string& tooltip,
                                                       std::function<void()> action);

    void onTrackChanged(const PlaylistEntry* entry);
    void onPlaybackStateChanged(bool playing, bool paused);
    void onPositionChanged(double seconds, double duration);
    void setClock(double seconds, double duration);

    app::Session& session_;

    mux::Window                          window_{nullptr};
    mux::Controls::TitleBar              titleBar_{nullptr};
    mux::Controls::FontIcon              playGlyph_{nullptr};
    mux::Controls::Button                playButton_{nullptr};
    mux::Controls::Slider                seek_{nullptr};
    mux::Controls::TextBlock             clock_{nullptr};
    mux::Controls::Slider                volume_{nullptr};
    mux::Controls::TextBlock             status_{nullptr};
    std::unique_ptr<PlaylistTable>       playlist_;

    double duration_ = 0.0;
    /// Set while the code moves a slider, so its ValueChanged is not taken for
    /// the listener's.
    bool settingSeek_   = false;
    bool settingVolume_ = false;
    /// The seek bar's pointer is down: the position stops following playback,
    /// and the seek happens on release.
    bool scrubbing_ = false;

    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::winui
