#include "Meters.hpp"

#include "Commands.hpp"
#include "SignalChain.hpp"
#include "Translations.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace xpcog::gtk {

namespace {

constexpr unsigned kFrameMs = 33;

[[nodiscard]] GdkRGBA foreground(GtkWidget* widget) {
    GdkRGBA colour{};
    gtk_widget_get_color(widget, &colour);
    return colour;
}

[[nodiscard]] GdkRGBA accent() {
    GdkRGBA colour{};
    AdwAccentColor chosen = adw_style_manager_get_accent_color(adw_style_manager_get_default());
    adw_accent_color_to_rgba(chosen, &colour);
    return colour;
}

void setSource(cairo_t* cr, const GdkRGBA& colour, double alpha = 1.0) {
    cairo_set_source_rgba(cr, colour.red, colour.green, colour.blue, colour.alpha * alpha);
}

/// Where a level falls along a bar, from 0 at the floor to 1 at full scale.
[[nodiscard]] double fraction(double decibels) {
    return std::clamp((decibels - LevelMeter::kFloor) / -LevelMeter::kFloor, 0.0, 1.0);
}

}  // namespace

// --- the VU meter ----------------------------------------------------------------

VuMeterView::VuMeterView(AudioTap& tap)
    : tap_(tap),
      owned_(GObjectPtr<GtkWidget>::sink(gtk_drawing_area_new())),
      area_(owned_.get()) {
    gtk_widget_set_size_request(area_, 200, 40);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area_),
        [](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<VuMeterView*>(data)->draw(cr, width, height);
        },
        this, nullptr);
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "map", [this](GtkWidget*) { setActive(active_); }));
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "unmap", [this](GtkWidget*) { timer_.stop(); }));

    // A click clears the clip lamps, as Cog's does.
    GtkGesture* click = gtk_gesture_click_new();
    connections_.push_back(Connection::to<void(GtkGestureClick*, int, double, double)>(
        click, "pressed", [this](GtkGestureClick*, int, double, double) {
            meter_.clearClip();
            gtk_widget_queue_draw(area_);
        }));
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
}

VuMeterView::~VuMeterView() {
    timer_.stop();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
}

void VuMeterView::setActive(bool active) {
    active_ = active;
    if (gtk_widget_get_mapped(area_)) {
        lastTick_ = std::chrono::steady_clock::now();
        timer_.start(kFrameMs, [this] { tick(); });
    } else {
        timer_.stop();
    }
}

void VuMeterView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    meter_.update(tap_, elapsed, sampleRate_ > 0.0 ? sampleRate_ : 44100.0, active_);
    gtk_widget_queue_draw(area_);

    const bool atRest = meter_.left().hold <= LevelMeter::kFloor &&
                        meter_.right().hold <= LevelMeter::kFloor;
    if (!active_ && atRest) {
        timer_.stop();
    }
}

void VuMeterView::draw(cairo_t* cr, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const GdkRGBA text = foreground(area_);
    const GdkRGBA fill = accent();

    constexpr double kMargin = 6.0;
    constexpr double kLabel  = 14.0;
    constexpr double kLamp   = 10.0;
    const double     left    = kMargin + kLabel;
    const double     right   = width - kMargin - kLamp - 4.0;
    const double     span    = std::max(1.0, right - left);
    const double     rowHeight =
        std::max(4.0, (height - kMargin * 3.0) / 2.0);

    const auto row = [&](int index, const char* name, const LevelMeter::Channel& channel) {
        const double top = kMargin + index * (rowHeight + kMargin);

        setSource(cr, text, 0.75);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, kMargin, top + rowHeight / 2.0 + 3.5);
        cairo_show_text(cr, name);

        setSource(cr, text, 0.12);
        cairo_rectangle(cr, left, top, span, rowHeight);
        cairo_fill(cr);

        setSource(cr, fill, 0.45);
        cairo_rectangle(cr, left, top, span * fraction(channel.peak), rowHeight);
        cairo_fill(cr);

        setSource(cr, fill);
        cairo_rectangle(cr, left, top, span * fraction(channel.rms), rowHeight);
        cairo_fill(cr);

        if (channel.hold > LevelMeter::kFloor) {
            setSource(cr, text);
            cairo_rectangle(cr, left + span * fraction(channel.hold) - 1.0, top, 2.0, rowHeight);
            cairo_fill(cr);
        }

        const GdkRGBA lamp = {0.88, 0.11, 0.14, 1.0};
        setSource(cr, channel.clipped ? lamp : text, channel.clipped ? 1.0 : 0.12);
        cairo_rectangle(cr, width - kMargin - kLamp, top, kLamp, rowHeight);
        cairo_fill(cr);
    };

    row(0, "L", meter_.left());
    row(1, "R", meter_.right());
}

// --- the Waveform pane -------------------------------------------------------------

EnvelopeView::EnvelopeView(AudioTap& tap)
    : tap_(tap),
      owned_(GObjectPtr<GtkWidget>::sink(gtk_drawing_area_new())),
      area_(owned_.get()) {
    gtk_widget_set_size_request(area_, 200, 48);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area_),
        [](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<EnvelopeView*>(data)->draw(cr, width, height);
        },
        this, nullptr);
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "map", [this](GtkWidget*) { setActive(active_); }));
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "unmap", [this](GtkWidget*) { timer_.stop(); }));
}

EnvelopeView::~EnvelopeView() {
    timer_.stop();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
}

void EnvelopeView::setSampleRate(double rate) {
    sampleRate_ = rate;
    cursor_.setSampleRate(rate);
    history_.setSampleRate(rate);
}

void EnvelopeView::setActive(bool active) {
    active_ = active;
    if (active && gtk_widget_get_mapped(area_)) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.start(kFrameMs, [this] { tick(); });
    } else {
        timer_.stop();
    }
}

void EnvelopeView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;
    if (sampleRate_ <= 0.0) {
        return;
    }

    const auto count = static_cast<std::size_t>(elapsed * sampleRate_);
    if (count == 0) {
        return;
    }
    scratch_.resize(count);
    if (cursor_.read(tap_, elapsed, scratch_.data(), count, TapLane::Mono)) {
        history_.push(scratch_.data(), count);
    } else {
        history_.pushSilence(count);
    }
    gtk_widget_queue_draw(area_);
}

void EnvelopeView::draw(cairo_t* cr, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const double middle = height / 2.0;
    setSource(cr, foreground(area_), 0.2);
    cairo_rectangle(cr, 0.0, middle - 0.5, width, 1.0);
    cairo_fill(cr);

    setSource(cr, accent());
    const std::size_t columns = history_.size();
    const double      step    = static_cast<double>(width) / static_cast<double>(columns);
    for (std::size_t i = 0; i < columns; ++i) {
        const EnvelopeHistory::Column& column = history_.at(i);
        const double top    = middle - static_cast<double>(std::clamp(column.high, -1.0F, 1.0F)) * middle;
        const double bottom = middle - static_cast<double>(std::clamp(column.low, -1.0F, 1.0F)) * middle;
        cairo_rectangle(cr, static_cast<double>(i) * step, top, std::max(step, 1.0),
                        std::max(bottom - top, 1.0));
    }
    cairo_fill(cr);
}

// --- the Signal Inspector ------------------------------------------------------------

SignalInspector::SignalInspector(GtkWindow* parent, app::Session& session) : session_(session) {
    window_ = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), app::tr("Signal Inspector").c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), 380, 560);
    gtk_window_set_transient_for(GTK_WINDOW(window_), parent);
    gtk_window_set_hide_on_close(GTK_WINDOW(window_), TRUE);

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(column, 12);
    gtk_widget_set_margin_end(column, 12);
    gtk_widget_set_margin_top(column, 6);
    gtk_widget_set_margin_bottom(column, 12);

    verdict_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(verdict_), 0.0F);
    gtk_widget_add_css_class(verdict_, "title-4");
    gtk_box_append(GTK_BOX(column), verdict_);

    stages_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(stages_), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(stages_, "boxed-list");
    gtk_box_append(GTK_BOX(column), stages_);

    meter_ = std::make_unique<VuMeterView>(session_.playback().tap());
    gtk_widget_set_size_request(meter_->widget(), -1, 56);
    gtk_widget_set_vexpand(meter_->widget(), FALSE);
    gtk_box_append(GTK_BOX(column), meter_->widget());

    live_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(live_), 0.0F);
    gtk_widget_add_css_class(live_, "dim-label");
    gtk_box_append(GTK_BOX(column), live_);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), column);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);
    adw_window_set_content(ADW_WINDOW(window_), view);

    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        window_, "notify::visible", [this](GObject*, GParamSpec*) {
            if (shown()) {
                refresh();
                timer_.start(250, [this] { refresh(); });
            } else {
                timer_.stop();
                meter_->setActive(false);
            }
            if (visibilityChanged) {
                visibilityChanged();
            }
        }));
}

SignalInspector::~SignalInspector() {
    timer_.stop();
    connections_.clear();
    meter_.reset();
    gtk_window_destroy(GTK_WINDOW(window_));
}

bool SignalInspector::shown() const { return gtk_widget_get_visible(window_) != FALSE; }

void SignalInspector::toggle() {
    if (shown()) {
        gtk_widget_set_visible(window_, FALSE);
    } else {
        gtk_window_present(GTK_WINDOW(window_));
    }
}

void SignalInspector::refresh() {
    const SignalReport     report = session_.playback().signalReport();
    const app::SignalChain chain  = app::describeSignal(report);

    gtk_label_set_text(GTK_LABEL(verdict_), chain.summary.c_str());
    meter_->setSampleRate(report.device.sampleRate);
    meter_->setActive(chain.active && !report.paused);

    std::string live;
    for (const std::string& line : chain.live) {
        live += (live.empty() ? "" : "\n") + line;
    }
    gtk_label_set_text(GTK_LABEL(live_), live.c_str());

    // Rebuilt only when something changed: this runs four times a second.
    std::string key;
    for (const app::SignalStage& stage : chain.stages) {
        key += stage.name + '\n' + stage.detail + '\n' +
               std::to_string(static_cast<int>(stage.state)) + '\n';
    }
    gtk_widget_set_visible(stages_, !chain.stages.empty());
    if (key == shownStages_) {
        return;
    }
    shownStages_ = std::move(key);

    gtk_list_box_remove_all(GTK_LIST_BOX(stages_));
    for (const app::SignalStage& stage : chain.stages) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), stage.name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), stage.detail.c_str());

        const char* icon = "object-select-symbolic";
        if (stage.state == app::SignalStage::State::Changing) {
            icon = "document-edit-symbolic";
        } else if (stage.state == app::SignalStage::State::Off) {
            gtk_widget_add_css_class(row, "dim-label");
            icon = nullptr;
        }
        if (icon != nullptr) {
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icon));
        }
        gtk_list_box_append(GTK_LIST_BOX(stages_), row);
    }
}

}  // namespace xpcog::gtk
