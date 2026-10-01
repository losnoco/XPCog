#include "SeekBar.hpp"

#include "Painting.hpp"
#include "Translations.hpp"

#include <adwaita.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace xpcog::gtk {
namespace {

// The geometry is the wx bar's, in logical pixels: GTK scales the cairo
// context for the display, so a "device-independent pixel" is a pixel here.
constexpr int kGrooveHeight = 4;
constexpr int kThumbRadius  = 6;
constexpr int kMargin       = kThumbRadius + 1;

constexpr int    kWaveformPadding = 2;
constexpr double kPlayheadWidth   = 2.0;

/// The bottom of the logarithmic scale; SeekBar.cpp in app/ says why -48.
constexpr double kLogFloorDb = -48.0;

constexpr float kPeakAlpha = 55.0F / 255.0F;
constexpr float kRmsAlpha  = 120.0F / 255.0F;

constexpr float kGrooveOutlineAlpha = 90.0F / 255.0F;
constexpr float kThumbOutlineAlpha  = 210.0F / 255.0F;
/// The unfilled groove: the text colour, faint. The wx bar reads 3DSHADOW,
/// which GTK has no name for; a shade of the foreground follows the theme
/// the same way.
constexpr float kGrooveAlpha = 0.22F;

/// The desktop's accent colour, which libadwaita reads from the portal and
/// falls back to its own blue for -- the answer platform::accentColour()
/// declines to give on Linux, where there is no desktop-wide value outside
/// GNOME's.
[[nodiscard]] GdkRGBA accent() {
    GdkRGBA* rgba = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    GdkRGBA  copy = *rgba;
    gdk_rgba_free(rgba);
    return copy;
}

void roundedRectangle(cairo_t* cr, double x, double y, double w, double h, double radius) {
    if (w <= 0.0 || h <= 0.0) {
        return;
    }
    const double r = std::min(radius, std::min(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

[[nodiscard]] bool same(const std::optional<GdkRGBA>& a, const std::optional<GdkRGBA>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a || gdk_rgba_equal(&*a, &*b);
}

}  // namespace

SeekBar::SeekBar() {
    root_  = gtk_stack_new();
    owned_ = GObjectPtr<GtkWidget>::sink(root_);
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_valign(root_, GTK_ALIGN_CENTER);
    gtk_stack_set_transition_type(GTK_STACK(root_), GTK_STACK_TRANSITION_TYPE_NONE);
    // Sized by what it shows, so the plain row is the slider's height and not
    // the waveform strip's.
    gtk_stack_set_vhomogeneous(GTK_STACK(root_), FALSE);

    // --- the slider ----------------------------------------------------------
    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(scale_), FALSE);
    gtk_widget_set_valign(scale_, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(scale_, 120, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(scale_), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   app::tr("Position").c_str(), -1);
    connections_.push_back(Connection::to<gboolean(GtkRange*, GtkScrollType, double)>(
        scale_, "change-value", [this](GtkRange*, GtkScrollType, double value) -> gboolean {
            if (duration_ <= 0.0) {
                return TRUE;  // nothing to seek within; leave the slider where it is
            }
            position_ = std::clamp(value, 0.0, duration_);
            if (pressed_) {
                // A drag, or the click that started one: follow it on the
                // clock, and seek when it lets go.
                scrubbing_ = true;
                scrubbed.publish(position_);
            } else {
                // The keyboard, or a scroll: one step, one seek.
                seekRequested.publish(position_);
            }
            return FALSE;
        }));
    GtkEventController* raw = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(raw, GTK_PHASE_CAPTURE);
    connections_.push_back(Connection::to<gboolean(GtkEventControllerLegacy*, GdkEvent*)>(
        raw, "event", [this](GtkEventControllerLegacy*, GdkEvent* event) -> gboolean {
            switch (gdk_event_get_event_type(event)) {
                case GDK_BUTTON_PRESS:
                case GDK_TOUCH_BEGIN:
                    pressed_   = true;
                    scrubbing_ = false;
                    break;
                case GDK_BUTTON_RELEASE:
                case GDK_TOUCH_END:
                case GDK_TOUCH_CANCEL:
                    if (pressed_) {
                        pressed_ = false;
                        // On release, not on every motion: seeking is
                        // expensive and a drag across a long track would ask
                        // the engine for a hundred. A cancelled touch was
                        // interrupted rather than completed, and seeks nothing.
                        const bool seek = scrubbing_ &&
                                          gdk_event_get_event_type(event) != GDK_TOUCH_CANCEL;
                        scrubbing_ = false;
                        if (seek) {
                            seekRequested.publish(position_);
                        }
                    }
                    break;
                default:
                    break;
            }
            return FALSE;  // seen, never taken: the scale does the rest
        }));
    gtk_widget_add_controller(scale_, raw);
    gtk_stack_add_named(GTK_STACK(root_), scale_, "plain");

    // --- the waveform ----------------------------------------------------------
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area_),
        [](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<SeekBar*>(data)->draw(cr, width, height);
        },
        this, nullptr);
    setWaveformMode(false);

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    connections_.push_back(Connection::to<void(GtkGestureDrag*, double, double)>(
        drag, "drag-begin", [this](GtkGestureDrag* gesture, double x, double) {
            if (duration_ <= 0.0) {
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
                return;
            }
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            scrubbing_ = true;
            // Straight to where the click landed, rather than paging towards
            // it. This is the behaviour the whole widget exists for.
            position_ = positionAt(x);
            scrubbed.publish(position_);
            gtk_widget_queue_draw(area_);
        }));
    connections_.push_back(Connection::to<void(GtkGestureDrag*, double, double)>(
        drag, "drag-update", [this](GtkGestureDrag* gesture, double offsetX, double) {
            if (!scrubbing_) {
                return;
            }
            double startX = 0.0;
            double startY = 0.0;
            gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
            position_ = positionAt(startX + offsetX);
            scrubbed.publish(position_);
            gtk_widget_queue_draw(area_);
        }));
    connections_.push_back(Connection::to<void(GtkGestureDrag*, double, double)>(
        drag, "drag-end", [this](GtkGestureDrag* gesture, double offsetX, double) {
            if (!scrubbing_) {
                return;
            }
            double startX = 0.0;
            double startY = 0.0;
            gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
            position_  = positionAt(startX + offsetX);
            scrubbing_ = false;
            // On release, not on every motion: seeking is expensive and a
            // drag across a long track would ask the engine for a hundred.
            seekRequested.publish(position_);
            gtk_widget_queue_draw(area_);
        }));
    connections_.push_back(Connection::to<void(GtkGesture*, GdkEventSequence*)>(
        drag, "cancel", [this](GtkGesture*, GdkEventSequence*) {
            // No seek published. The gesture was interrupted rather than
            // completed, and jumping the track because a dialog stole the
            // pointer is not what was asked for.
            scrubbing_ = false;
            gtk_widget_queue_draw(area_);
        }));
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));
    gtk_stack_add_named(GTK_STACK(root_), area_, "waveform");

    // Every colour this draws is read at paint time, so a repaint is the
    // whole of what an appearance change needs -- but something has to ask
    // for it, and a bar sitting at 0:00 has no other reason to redraw.
    AdwStyleManager* style = adw_style_manager_get_default();
    for (const char* property : {"notify::dark", "notify::accent-color-rgba"}) {
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            style, property, [this](GObject*, GParamSpec*) { gtk_widget_queue_draw(area_); }));
    }

    syncScale();
    choose();
}

void SeekBar::syncScale() {
    // Set, never emitted as a change: gtk_range_set_value() fires
    // value-changed but not change-value, which is the signal that means the
    // listener moved it.
    gtk_widget_set_sensitive(scale_, duration_ > 0.0);
    gtk_range_set_range(GTK_RANGE(scale_), 0.0, duration_ > 0.0 ? duration_ : 1.0);
    // Arrow keys step five seconds and Page keys thirty, which is what a
    // listener reaching for the keyboard wants rather than a percentage.
    gtk_range_set_increments(GTK_RANGE(scale_), 5.0, 30.0);
    gtk_range_set_value(GTK_RANGE(scale_), duration_ > 0.0 ? position_ : 0.0);
}

void SeekBar::choose() {
    const bool painted =
        waveformMode_ && waveform_ && duration_ > 0.0 && waveform_->bucketCount > 0;
    gtk_stack_set_visible_child(GTK_STACK(root_), painted ? area_ : scale_);
}

SeekBar::~SeekBar() {
    // The draw function holds `this`; a draw after this object has gone would
    // read freed memory.
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
}

void SeekBar::setDuration(double seconds) {
    const double clamped = seconds > 0.0 ? seconds : 0.0;
    if (duration_ == clamped) {
        return;
    }
    duration_ = clamped;
    if (position_ > duration_) {
        position_ = duration_;
    }
    syncScale();
    choose();
    gtk_widget_queue_draw(area_);
}

void SeekBar::setPosition(double seconds) {
    if (scrubbing_) {
        return;
    }
    const double clamped = std::clamp(seconds, 0.0, duration_);
    if (position_ == clamped) {
        return;
    }
    position_ = clamped;
    if (!pressed_) {
        gtk_range_set_value(GTK_RANGE(scale_), position_);
    }
    gtk_widget_queue_draw(area_);
}

SeekBar::WaveformStyle SeekBar::styleFrom(const Settings& settings) {
    return {.rectified   = settings.WaveformRectified(),
            .logarithmic = settings.WaveformLogScale(),
            .height      = std::clamp(settings.WaveformHeight(), 20, 80),
            .played      = parseColour(settings.WaveformPlayedColor()),
            .unplayed    = parseColour(settings.WaveformUnplayedColor())};
}

void SeekBar::setWaveformMode(bool on) {
    waveformMode_ = on;
    // The strip keeps its height while the shape is still coming, so the row
    // does not jump when it arrives; the slider sits centred in it meanwhile.
    gtk_widget_set_size_request(area_, 120, style_.height);
    gtk_widget_set_size_request(root_, -1, on ? style_.height : -1);
    choose();
    gtk_widget_queue_draw(area_);
}

void SeekBar::setWaveformStyle(const WaveformStyle& style) {
    if (style.rectified == style_.rectified && style.logarithmic == style_.logarithmic &&
        style.height == style_.height && same(style.played, style_.played) &&
        same(style.unplayed, style_.unplayed)) {
        return;
    }
    const bool grew = style.height != style_.height;
    style_          = style;
    if (waveformMode_) {
        if (grew) {
            setWaveformMode(true);  // re-applies the size request
        }
        gtk_widget_queue_draw(area_);
    }
}

void SeekBar::setWaveform(std::shared_ptr<const WaveformSummary> summary) {
    waveform_ = std::move(summary);
    choose();
    if (waveformMode_) {
        gtk_widget_queue_draw(area_);
    }
}

double SeekBar::positionAt(double x) const {
    const double width = gtk_widget_get_width(area_) - (2.0 * kMargin);
    if (width <= 0.0 || duration_ <= 0.0) {
        return 0.0;
    }
    const double fraction = std::clamp((x - kMargin) / width, 0.0, 1.0);
    return fraction * duration_;
}

double SeekBar::thumbCentre() const {
    const double width = gtk_widget_get_width(area_) - (2.0 * kMargin);
    if (width <= 0.0) {
        return kMargin;
    }
    const double fraction = duration_ > 0.0 ? position_ / duration_ : 0.0;
    return kMargin + std::floor(fraction * width);
}

void SeekBar::draw(cairo_t* cr, int width, int height) {
    const double centreY = height / 2.0;
    const double left    = kMargin;
    const double span    = std::max(0, width - (2 * kMargin));
    // Shown only with a shape to draw; choose() puts the slider up otherwise.
    if (!waveform_ || duration_ <= 0.0 || waveform_->bucketCount == 0) {
        return;
    }
    const double halfHeight = std::max(1.0, centreY - kWaveformPadding);
    paintWaveform(cr, left, span, centreY, halfHeight);
}

void SeekBar::paintWaveform(cairo_t* cr, double left, double width, double centreY,
                            double halfHeight) {
    const WaveformSummary& shape   = *waveform_;
    const auto             columns = static_cast<int>(width);
    if (columns <= 0) {
        return;
    }

    const GdkRGBA fg           = foreground(area_);
    const GdkRGBA trackColour  = withAlpha(fg, kGrooveAlpha);
    const GdkRGBA accentColour = style_.played.value_or(accent());
    const GdkRGBA playedPeak   = withAlpha(accentColour, kPeakAlpha + (60.0F / 255.0F));
    const GdkRGBA playedRms    = accentColour;
    const double  hairline     = 1.0;

    // The unplayed part: a shade of the text colour unless a colour was
    // chosen, because the text colour is the one thing guaranteed to read
    // against the window in either appearance.
    const auto unplayed = [&](float alpha) {
        return withAlpha(style_.unplayed.value_or(fg), alpha);
    };

    // Where the shape stops and the plain groove begins, in columns. A
    // bucket is drawn once every column it covers has been analysed.
    const double bucketsPerColumn = static_cast<double>(shape.bucketCount) / columns;
    const int    analysedColumns =
        std::clamp(static_cast<int>(std::floor(shape.analysed / bucketsPerColumn)), 0, columns);
    const int playedColumns =
        std::clamp(static_cast<int>(thumbCentre() - left), 0, columns);

    const bool   rectified = style_.rectified;
    const double baseline  = rectified ? centreY + halfHeight : centreY;
    const double amplitude = rectified ? 2.0 * halfHeight : halfHeight;

    // The baseline first, under everything, so a silent stretch still reads
    // as part of the bar rather than a gap in it.
    setSource(cr, trackColour);
    cairo_set_line_width(cr, hairline);
    cairo_move_to(cr, left, baseline + 0.5);
    cairo_line_to(cr, left + width, baseline + 0.5);
    cairo_stroke(cr);

    const auto scale = [&](std::uint8_t value) {
        if (value == 0) {
            return 0.0;
        }
        const double linear = value / 255.0;
        if (!style_.logarithmic) {
            return linear;
        }
        const double db = 20.0 * std::log10(linear);
        return std::clamp(1.0 - (db / kLogFloorDb), 0.0, 1.0);
    };

    const auto level = [&](const std::vector<std::uint8_t>& values, int x) {
        const int    first = static_cast<int>(x * bucketsPerColumn);
        const int    last  = std::max(first, static_cast<int>((x + 1) * bucketsPerColumn) - 1);
        std::uint8_t peak  = 0;
        for (int i = first; i <= last && i < static_cast<int>(values.size()); ++i) {
            peak = std::max(peak, values[i]);
        }
        return scale(peak) * amplitude;
    };

    // One polygon per (level, played) pair: across the top edge of every
    // column, then back along the bottom, filled in one go so there are no
    // seams between columns.
    const auto fill = [&](const std::vector<std::uint8_t>& values, int from, int to,
                          const GdkRGBA& colour) {
        if (to <= from) {
            return;
        }
        cairo_new_path(cr);
        cairo_move_to(cr, left + from, baseline);
        for (int x = from; x < to; ++x) {
            const double h = level(values, x);
            cairo_line_to(cr, left + x, baseline - h);
            cairo_line_to(cr, left + x + 1, baseline - h);
        }
        cairo_line_to(cr, left + to, baseline);
        if (!rectified) {
            for (int x = to - 1; x >= from; --x) {
                const double h = level(values, x);
                cairo_line_to(cr, left + x + 1, baseline + h);
                cairo_line_to(cr, left + x, baseline + h);
            }
        }
        cairo_close_path(cr);
        setSource(cr, colour);
        cairo_fill(cr);
    };

    const int split = std::min(playedColumns, analysedColumns);
    fill(shape.peak, 0, split, playedPeak);
    fill(shape.peak, split, analysedColumns, unplayed(kPeakAlpha));
    fill(shape.rms, 0, split, playedRms);
    fill(shape.rms, split, analysedColumns, unplayed(kRmsAlpha));

    // Past the analysis: the plain groove, with the played part filled if the
    // playhead has got there first.
    if (analysedColumns < columns) {
        const double top  = rectified ? baseline - kGrooveHeight : centreY - (kGrooveHeight / 2.0);
        const double x    = left + analysedColumns;
        const double rest = width - analysedColumns;
        roundedRectangle(cr, x + (hairline / 2.0), top + (hairline / 2.0),
                         std::max(0.0, rest - hairline), std::max(0.0, kGrooveHeight - hairline),
                         kGrooveHeight / 2.0);
        setSource(cr, trackColour);
        cairo_fill_preserve(cr);
        setSource(cr, withAlpha(fg, kGrooveOutlineAlpha));
        cairo_set_line_width(cr, hairline);
        cairo_stroke(cr);
        if (playedColumns > analysedColumns) {
            roundedRectangle(cr, x + (hairline / 2.0), top + (hairline / 2.0),
                             std::max(0.0, playedColumns - analysedColumns - hairline),
                             std::max(0.0, kGrooveHeight - hairline), kGrooveHeight / 2.0);
            setSource(cr, accentColour);
            cairo_fill(cr);
        }
    }

    // The playhead, full height, in the foreground colour: it has to read
    // over the accent on its left and the grey on its right.
    setSource(cr, withAlpha(fg, kThumbOutlineAlpha));
    cairo_rectangle(cr, thumbCentre() - (kPlayheadWidth / 2.0), centreY - halfHeight,
                    kPlayheadWidth, 2.0 * halfHeight);
    cairo_fill(cr);
}

}  // namespace xpcog::gtk
