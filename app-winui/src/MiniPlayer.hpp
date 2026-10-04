#pragma once

// The mini player. Port of Cog's `miniWindow` (Base.lproj/MainMenu.xib, driven
// by AppController's -setMiniMode:), by way of the wx player's MiniFrame.
//
// A mode, not a second window: the main window hides while this shows, and
// closing this one goes back rather than quitting -- the point is to get the
// playlist off the screen, as Cog's -toggleMiniMode: has it.
//
// Here it can be what Cog's is, which the wx and GTK ones could not. Cog's mini
// window has no content view at all -- it is a window whose whole body is a
// toolbar in its title bar -- and the wx MiniFrame put the same controls in a
// row under a title bar the window manager drew, because no toolkit there let
// the controls into it. WinUI does: the content is extended into the title bar
// and the transport is the title bar's content, so the window is its title bar
// and nothing else. The height is pinned to it; the width is the listener's,
// and is what the seek bar uses.
//
// Cog's `miniPlusWindow`, the larger variant with artwork, is not ported --
// the wx player's decision, kept.

#include "SeekBar.hpp"
#include "WinRT.hpp"

#include <winrt/Microsoft.UI.Xaml.Documents.h>

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/Waveform.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xpcog::app {
class Session;
}

namespace xpcog::winui {

class MiniPlayer {
public:
    explicit MiniPlayer(app::Session& session);
    ~MiniPlayer();

    MiniPlayer(const MiniPlayer&)            = delete;
    MiniPlayer& operator=(const MiniPlayer&) = delete;

    void show();
    void hide();
    [[nodiscard]] bool shown() const noexcept { return shown_; }

    /// Closes the window for good: the player is quitting. Anything else that
    /// closes it -- its close button, Alt+F4 -- goes back to the full window.
    void close();

    /// The track, as the window's title (the taskbar's) -- "title -- artist",
    /// the wx MiniFrame's form -- and over the seek bar, the window having no
    /// title text of its own to show it in.
    void setNowPlaying(const std::string& title, const std::string& artist);
    void setPlaybackState(bool playing, bool paused);
    void setPosition(double seconds, double duration);
    /// The volume moved elsewhere: the main window, a media key, the remote.
    void setVolume(double gain);
    void setWaveform(std::shared_ptr<const WaveformSummary> summary);
    /// The waveform setting or its style changed.
    void applyWaveformSetting();
    /// Above every other window, or not: Cog's `floatingMiniWindow`.
    void setFloating(bool floating);

    [[nodiscard]] HWND hwnd() const;

    /// The listener closed it: go back to the full window.
    std::function<void()> dismissed;
    /// The volume slider moved, so the main window's can follow.
    std::function<void(double)> volumeChanged;

private:
    static LRESULT CALLBACK subclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR self);
    /// The client height the window is pinned to, in physical pixels.
    [[nodiscard]] int clientHeight() const;
    /// Shows the track over the seek bar, or fades it while the bar is in use.
    void updateOverlay();

    app::Session& session_;

    mux::Window              window_{nullptr};
    HWND                     hwnd_ = nullptr;
    mux::Controls::Button    playButton_{nullptr};
    std::unique_ptr<SeekBar> seekBar_;
    mux::Controls::TextBlock clock_{nullptr};
    mux::Controls::Slider    volume_{nullptr};
    mux::Controls::Grid      overlay_{nullptr};
    mux::Documents::Run      overlayTitle_{nullptr};
    mux::Documents::Run      overlayArtist_{nullptr};

    double duration_       = 0.0;
    /// The pointer is over the seek bar.
    bool   hovering_       = false;
    bool   shown_          = false;
    bool   closing_        = false;
    bool   settingVolume_  = false;
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::winui
