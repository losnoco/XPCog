#include "MiniWindow.hpp"

#include "TrackText.hpp"

#include <utility>

namespace xpcog::gtk {

namespace {

constexpr const char* kResource = "/co/losno/XPCog/ui/mini.ui";

template <typename T>
T* object(GtkBuilder* builder, const char* id) {
    GObject* found = gtk_builder_get_object(builder, id);
    if (found == nullptr) {
        g_error("%s has no object named %s", kResource, id);
    }
    return reinterpret_cast<T*>(found);
}

}  // namespace

MiniWindow::MiniWindow(AdwApplication* application, GActionGroup* actions) {
    builder_    = GObjectPtr<GtkBuilder>::adopt(gtk_builder_new_from_resource(kResource));
    window_     = object<GtkWindow>(builder_.get(), "mini_window");
    title_      = object<AdwWindowTitle>(builder_.get(), "mini_title");
    playButton_ = object<GtkButton>(builder_.get(), "mini_play_button");
    clock_      = object<GtkLabel>(builder_.get(), "mini_clock");
    volume_     = object<GtkAdjustment>(builder_.get(), "mini_volume_adjustment");

    // The main window's actions, under the prefix the buttons name. The
    // application's accelerators resolve through the same lookup, so the
    // shortcuts work here too.
    gtk_widget_insert_action_group(GTK_WIDGET(window_), "win", actions);
    gtk_window_set_application(window_, GTK_APPLICATION(application));
    gtk_window_set_icon_name(window_, "co.losno.XPCog");

    // Closing means "back to the full window", not quitting: the mode is one
    // of the two windows on screen, never neither.
    connections_.push_back(Connection::to<gboolean(GtkWindow*)>(
        window_, "close-request", [this](GtkWindow*) -> gboolean {
            dismissed.publish();
            return TRUE;
        }));

    seekBar_ = std::make_unique<SeekBar>();
    gtk_box_append(GTK_BOX(object<GtkBox>(builder_.get(), "mini_seek_slot")), seekBar_->widget());
    subscriptions_.push_back(
        seekBar_->seekRequested.connect([this](double seconds) { seekRequested.publish(seconds); }));
    subscriptions_.push_back(
        seekBar_->scrubbed.connect([this](double seconds) { setClock(seconds, duration_); }));

    connections_.push_back(Connection::to<void(GtkAdjustment*)>(
        volume_, "value-changed", [this](GtkAdjustment* adjustment) {
            if (!settingScales_) {
                volumeChanged.publish(gtk_adjustment_get_value(adjustment) / 100.0);
            }
        }));
}

MiniWindow::~MiniWindow() {
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
    }
}

void MiniWindow::present() { gtk_window_present(window_); }

void MiniWindow::hide() { gtk_widget_set_visible(GTK_WIDGET(window_), FALSE); }

bool MiniWindow::visible() const { return gtk_widget_get_visible(GTK_WIDGET(window_)); }

GtkWidget* MiniWindow::widget() const { return GTK_WIDGET(window_); }

void MiniWindow::setNowPlaying(const std::string& title, const std::string& artist) {
    adw_window_title_set_title(title_, title.empty() ? "XPCog" : title.c_str());
    adw_window_title_set_subtitle(title_, artist.c_str());
    gtk_window_set_title(window_, title.empty() ? "XPCog" : (title + " \xE2\x80\x94 XPCog").c_str());
}

void MiniWindow::setPosition(double seconds, double duration) {
    duration_ = duration;
    seekBar_->setDuration(duration);
    seekBar_->setPosition(seconds);
    if (!seekBar_->scrubbing()) {
        setClock(seconds, duration);
    }
}

void MiniWindow::setPlaybackState(bool playing, bool paused) {
    gtk_button_set_icon_name(playButton_, playing && !paused ? "media-playback-pause-symbolic"
                                                             : "media-playback-start-symbolic");
    if (!playing) {
        setPosition(0.0, 0.0);
    }
}

void MiniWindow::setWaveformMode(bool on) { seekBar_->setWaveformMode(on); }

void MiniWindow::setWaveformStyle(const SeekBar::WaveformStyle& style) {
    seekBar_->setWaveformStyle(style);
}

void MiniWindow::setWaveform(std::shared_ptr<const WaveformSummary> summary) {
    seekBar_->setWaveform(std::move(summary));
}

void MiniWindow::setVolume(double gain) {
    settingScales_ = true;
    gtk_adjustment_set_value(volume_, gain * 100.0);
    settingScales_ = false;
}

void MiniWindow::setClock(double seconds, double duration) {
    const std::string text = app::formatClock(seconds) + " / " + app::formatClock(duration);
    gtk_label_set_text(clock_, text.c_str());
}

}  // namespace xpcog::gtk
