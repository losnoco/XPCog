// The mini player: the counterpart of app/src/MiniFrame.hpp.
//
// A second window over the same actions as the main one -- the main window's
// action group is inserted here under the same "win" prefix, so the buttons
// and the shortcuts reach the one switch -- showing the track, the transport,
// a seek bar and the volume. It cannot stay above other windows: GTK 4 has
// no keep-above and Wayland would not honour one, which docs/GTKPORT.md
// records; the Appearance row for it is shown greyed.

#pragma once

#include "Glib.hpp"
#include "SeekBar.hpp"

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/audio/Waveform.hpp"

#include <adwaita.h>

#include <memory>
#include <string>
#include <vector>

namespace xpcog::gtk {

class MiniWindow {
public:
    /// `actions` is the main window, whose "win." actions the buttons use.
    MiniWindow(AdwApplication* application, GActionGroup* actions);
    ~MiniWindow();

    MiniWindow(const MiniWindow&)            = delete;
    MiniWindow& operator=(const MiniWindow&) = delete;

    void present();
    void hide();
    [[nodiscard]] bool visible() const;
    /// The window itself, for a dialog that needs a parent.
    [[nodiscard]] GtkWidget* widget() const;

    void setNowPlaying(const std::string& title, const std::string& artist);
    void setPosition(double seconds, double duration);
    void setPlaybackState(bool playing, bool paused);
    void setVolume(double gain);

    /// The seek bar's waveform mode, style and shape, forwarded: the bar is
    /// the same widget the main window has, and a fresh mini window opens
    /// mid-track with a plain bar until it is handed the shape.
    void setWaveformMode(bool on);
    void setWaveformStyle(const SeekBar::WaveformStyle& style);
    void setWaveform(std::shared_ptr<const WaveformSummary> summary);

    /// The window was closed: back to the full window, as in Cog.
    Signal<> dismissed;
    /// The listener moved the seek scale.
    Signal<double> seekRequested;
    /// The listener moved the volume.
    Signal<double> volumeChanged;

private:
    void setClock(double seconds, double duration);

    GObjectPtr<GtkBuilder> builder_;
    GtkWindow*             window_     = nullptr;
    AdwWindowTitle*        title_      = nullptr;
    GtkButton*             playButton_ = nullptr;
    GtkLabel*              clock_      = nullptr;
    GtkAdjustment*         volume_     = nullptr;
    std::unique_ptr<SeekBar> seekBar_;

    double duration_      = 0.0;
    bool   settingScales_ = false;

    std::vector<Connection>   connections_;
    std::vector<Subscription> subscriptions_;
};

}  // namespace xpcog::gtk
