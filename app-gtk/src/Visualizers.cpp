#include "Visualizers.hpp"

#include "Painting.hpp"
#include "Translations.hpp"

#include "xpcog/core/audio/Oscilloscope.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace xpcog::gtk {

using app::tr;

// --- the menu ----------------------------------------------------------------------

VisualizerMenu::VisualizerMenu(GtkWidget* area, const char* prefix)
    : area_(area),
      group_(GObjectPtr<GSimpleActionGroup>::adopt(g_simple_action_group_new())),
      menu_(GObjectPtr<GMenu>::adopt(g_menu_new())) {
    gtk_widget_insert_action_group(area_, prefix, G_ACTION_GROUP(group_.get()));

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    connections_.push_back(Connection::to<void(GtkGestureClick*, int, double, double)>(
        click, "pressed", [this](GtkGestureClick* gesture, int, double x, double y) {
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            popup(x, y);
        }));
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
}

VisualizerMenu::~VisualizerMenu() {
    // A popover parented by hand is unparented by hand, before the area it
    // is parented to goes.
    if (popover_ != nullptr) {
        gtk_widget_unparent(popover_);
        popover_ = nullptr;
    }
}

void VisualizerMenu::build() {
    popover_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu_.get()));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), FALSE);
    gtk_widget_set_halign(popover_, GTK_ALIGN_START);
    gtk_widget_set_parent(popover_, area_);
}

void VisualizerMenu::popup(double x, double y) {
    if (popover_ == nullptr) {
        return;
    }
    if (sync) {
        sync();
    }
    const GdkRectangle at{static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &at);
    gtk_popover_popup(GTK_POPOVER(popover_));
}

namespace {

/// A stateful action whose state is a string chosen from a menu's targets:
/// the channel modes. `apply` gets the chosen key.
template <typename F>
void addChoiceAction(GSimpleActionGroup* group, const char* name, std::vector<Connection>& keep,
                     F apply) {
    GSimpleAction* action =
        g_simple_action_new_stateful(name, G_VARIANT_TYPE_STRING, g_variant_new_string("mono"));
    keep.push_back(Connection::to<void(GSimpleAction*, GVariant*)>(
        action, "activate", [apply](GSimpleAction* self, GVariant* parameter) {
            if (parameter == nullptr) {
                return;
            }
            g_simple_action_set_state(self, parameter);
            apply(g_variant_get_string(parameter, nullptr));
        }));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
}

/// A boolean action that flips on activation: the check items. `flip`
/// flips the *setting* and answers the new value; the action's state
/// follows it, rather than the other way round, so a setting changed in
/// Preferences while the menu was closed is what the next flip starts from.
template <typename F>
void addToggleAction(GSimpleActionGroup* group, const char* name, std::vector<Connection>& keep,
                     F flip) {
    GSimpleAction* action = g_simple_action_new_stateful(name, nullptr, g_variant_new_boolean(FALSE));
    keep.push_back(Connection::to<void(GSimpleAction*, GVariant*)>(
        action, "activate", [flip](GSimpleAction* self, GVariant*) {
            g_simple_action_set_state(self, g_variant_new_boolean(flip()));
        }));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
}

template <typename F>
void addPlainAction(GSimpleActionGroup* group, const char* name, std::vector<Connection>& keep,
                    F apply) {
    GSimpleAction* action = g_simple_action_new(name, nullptr);
    keep.push_back(Connection::to<void(GSimpleAction*, GVariant*)>(
        action, "activate", [apply](GSimpleAction*, GVariant*) { apply(); }));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
}

void setChoiceState(GSimpleActionGroup* group, const char* name, const char* key) {
    if (GAction* action = g_action_map_lookup_action(G_ACTION_MAP(group), name)) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(key));
    }
}

void setToggleState(GSimpleActionGroup* group, const char* name, bool on) {
    if (GAction* action = g_action_map_lookup_action(G_ACTION_MAP(group), name)) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(on));
    }
}

void appendChoice(GMenu* menu, const std::string& label, const char* action, const char* target) {
    GMenuItem* item = g_menu_item_new(label.c_str(), nullptr);
    g_menu_item_set_action_and_target_value(item, action, g_variant_new_string(target));
    g_menu_append_item(menu, item);
    g_object_unref(item);
}

/// The "Preferences…" row every visualiser's menu ends with. The same
/// string the Pitch & Tempo pane's button uses, so it is already translated.
[[nodiscard]] std::string preferencesLabel() { return tr("Preferences\xE2\x80\xA6"); }

/// 60 Hz. See SpectrumPanel.cpp.
constexpr unsigned kSpectrumFrameMs = 16;
constexpr int      kGapDenominator  = 3;
constexpr int      kGridLinesDb[]   = {-10, -20, -30, -40, -50, -60, -70};
/// The spectrum's background: its own rather than the theme's, or bars on a
/// light panel in a dark theme is what inheriting it produces.
const GdkRGBA kSpectrumBackground{18.0F / 255.0F, 18.0F / 255.0F, 20.0F / 255.0F, 1.0F};
constexpr float kGridAlpha  = 22.0F / 255.0F;
constexpr float kSplitAlpha = 48.0F / 255.0F;
constexpr int   kFrequencyBarPitch = 5;

/// The scope's room above and below a full-scale trace, its tap margin and
/// its two alphas; OscilloscopePanel.cpp says why each.
constexpr int         kScopePadding = 2;
constexpr std::size_t kTapMargin    = 1024;
constexpr float       kFillAlpha    = 90.0F / 255.0F;
constexpr float       kCentreAlpha  = 70.0F / 255.0F;

}  // namespace

// --- the spectrum ------------------------------------------------------------------

SpectrumView::SpectrumView(AudioTap& tap, Settings& settings)
    : tap_(tap),
      settings_(settings),
      owned_(GObjectPtr<GtkWidget>::sink(gtk_drawing_area_new())),
      area_(owned_.get()),
      menu_(area_, "spectrum") {
    for (std::vector<float>& window : windows_) {
        window.assign(SpectrumAnalyzer::kWindowFrames, 0.0F);
    }
    gtk_widget_set_size_request(area_, 200, 48);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area_),
        [](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<SpectrumView*>(data)->draw(cr, width, height);
        },
        this, nullptr);
    // Only matters in Frequencies mode, where the bar count follows the
    // width. NoteBands has a fixed series and ignores it.
    connections_.push_back(Connection::to<void(GtkDrawingArea*, int, int)>(
        area_, "resize", [this](GtkDrawingArea*, int, int) { updateFrequencyBandCount(); }));
    // The strip reveals and hides sections; the clock follows the widget on
    // and off the screen so the owner has only playback to report.
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "map", [this](GtkWidget*) { setActive(playing_); }));
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "unmap", [this](GtkWidget*) { timer_.stop(); }));

    buildMenu();
    applySettings(settings_);
}

SpectrumView::~SpectrumView() {
    timer_.stop();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
}

void SpectrumView::buildMenu() {
    // The two choices that get flipped while looking at the bars -- which
    // channels, and whether the peaks are drawn -- and a way to the rest.
    // Cog has no such menu, so this is an addition rather than a ported
    // behaviour, and it is the wx panel's addition, row for row.
    GSimpleActionGroup* group = menu_.actions();
    addChoiceAction(group, "channels", connections_, [this](const char* key) {
        settings_.setSpectrumChannels(key);
        settingChanged.publish("spectrumChannels");
    });
    addToggleAction(group, "peaks", connections_, [this] {
        settings_.setSpectrumShowPeaks(!settings_.SpectrumShowPeaks());
        settingChanged.publish("spectrumShowPeaks");
        return settings_.SpectrumShowPeaks();
    });
    addPlainAction(group, "preferences", connections_, [this] { settingsRequested.publish(); });

    GMenu* model = menu_.model();
    GMenu* modes = g_menu_new();
    appendChoice(modes, tr("Mono"), "spectrum.channels", "mono");
    appendChoice(modes, tr("Left"), "spectrum.channels", "left");
    appendChoice(modes, tr("Right"), "spectrum.channels", "right");
    appendChoice(modes, tr("Stereo, mirrored"), "spectrum.channels", "mirrored");
    appendChoice(modes, tr("Stereo, stacked"), "spectrum.channels", "stacked");
    appendChoice(modes, tr("Stereo, overlaid"), "spectrum.channels", "overlaid");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(modes));
    g_object_unref(modes);
    GMenu* flags = g_menu_new();
    g_menu_append(flags, tr("Show peak markers").c_str(), "spectrum.peaks");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(flags));
    g_object_unref(flags);
    GMenu* rest = g_menu_new();
    g_menu_append(rest, preferencesLabel().c_str(), "spectrum.preferences");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(rest));
    g_object_unref(rest);

    // Ticks read from settings when the menu opens, so it says what the pane says.
    menu_.sync = [this, group] {
        setChoiceState(group, "channels", settings_.SpectrumChannels().c_str());
        setToggleState(group, "peaks", settings_.SpectrumShowPeaks());
    };
    menu_.build();
}

void SpectrumView::setSampleRate(double rate) {
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.prepare(rate);
    }
    cursor_.setSampleRate(rate);
}

void SpectrumView::applySettings(const Settings& settings) {
    // Invalid colour text keeps whatever was there rather than turning black,
    // which on a near-black background reads as the spectrum having stopped.
    if (const auto bar = parseColour(settings.SpectrumBarColor())) {
        barColor_ = *bar;
    }
    if (const auto peak = parseColour(settings.SpectrumDotColor())) {
        peakColor_ = *peak;
    }
    showPeaks_ = settings.SpectrumShowPeaks();

    const Channels channels = app::spectrumChannelsFromKey(settings.SpectrumChannels());
    if (channels != channels_) {
        channels_ = channels;
        // The analysers hold the last frame's levels and peaks, read from a
        // lane the new mode may not show. Cleared rather than left to decay.
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
    }
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFloorDb(settings.SpectrumFloorDb());
        analyzer.setMode(settings.SpectrumFreqMode() ? SpectrumAnalyzer::Mode::Frequencies
                                                     : SpectrumAnalyzer::Mode::NoteBands);
    }
    updateFrequencyBandCount();
    gtk_widget_queue_draw(area_);
}

void SpectrumView::updateFrequencyBandCount() {
    if (analyzers_[0].mode() != SpectrumAnalyzer::Mode::Frequencies) {
        return;
    }
    const auto bars =
        static_cast<std::size_t>(std::max(1, gtk_widget_get_width(area_) / kFrequencyBarPitch));
    for (SpectrumAnalyzer& analyzer : analyzers_) {
        analyzer.setFrequencyBandCount(bars);
    }
}

void SpectrumView::setActive(bool active) {
    playing_ = active;
    if (!active) {
        for (SpectrumAnalyzer& analyzer : analyzers_) {
            analyzer.reset();
        }
        gtk_widget_queue_draw(area_);
    }
    // Visible *and* playing, or the clock stops.
    if (active && gtk_widget_get_mapped(area_)) {
        // The cursor starts again from wherever the audio has got to: every
        // path here has a gap behind it that the display did not draw.
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.start(kSpectrumFrameMs, [this] { tick(); });
    } else {
        timer_.stop();
    }
}

void SpectrumView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    const bool    both  = app::stereo(channels_);
    const TapLane first = (channels_ == Channels::Mono)    ? TapLane::Mono
                          : (channels_ == Channels::Right) ? TapLane::Right
                                                           : TapLane::Left;

    // Nothing has played yet, or the writer lapped the window: the last frame
    // stays on screen rather than a flicker to silence.
    if (!cursor_.read(tap_, elapsed, windows_[0].data(), windows_[0].size(), first)) {
        return;
    }
    if (both && !cursor_.readAgain(tap_, windows_[1].data(), windows_[1].size(), TapLane::Right)) {
        return;
    }
    analyzers_[0].analyze(windows_[0].data(), windows_[0].size());
    if (both) {
        analyzers_[1].analyze(windows_[1].data(), windows_[1].size());
    }
    gtk_widget_queue_draw(area_);
}

void SpectrumView::draw(cairo_t* cr, int widthPx, int heightPx) {
    setSource(cr, kSpectrumBackground);
    cairo_paint(cr);

    if (analyzers_[0].bands().empty()) {
        return;
    }
    const auto width  = static_cast<double>(widthPx);
    const auto height = static_cast<double>(heightPx);
    if (width <= 0.0 || height <= 0.0) {
        return;
    }

    const SpectrumAnalyzer& left  = analyzers_[0];
    const SpectrumAnalyzer& right = analyzers_[1];
    const GdkRGBA           split = withAlpha({1.0F, 1.0F, 1.0F, 1.0F}, kSplitAlpha);

    switch (channels_) {
        case Channels::Mono:
        case Channels::Left:
        case Channels::Right:
            paintGrid(cr, width, 0.0, height, false);
            paintBars(cr, width, left, 0.0, height, false, barColor_, peakColor_);
            break;
        case Channels::Mirrored: {
            // Left rises from the centre line, right falls from it.
            const double half = std::floor(height / 2.0);
            paintGrid(cr, width, 0.0, half, false);
            paintGrid(cr, width, half, height - half, true);
            paintBars(cr, width, left, 0.0, half, false, barColor_, peakColor_);
            paintBars(cr, width, right, half, height - half, true, barColor_, peakColor_);
            setSource(cr, split);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 0.0, half + 0.5);
            cairo_line_to(cr, width, half + 0.5);
            cairo_stroke(cr);
            break;
        }
        case Channels::Stacked: {
            const double half = std::floor(height / 2.0);
            paintGrid(cr, width, 0.0, half, false);
            paintGrid(cr, width, half, height - half, false);
            paintBars(cr, width, left, 0.0, half, false, barColor_, peakColor_);
            paintBars(cr, width, right, half, height - half, false, barColor_, peakColor_);
            setSource(cr, split);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 0.0, half + 0.5);
            cairo_line_to(cr, width, half + 0.5);
            cairo_stroke(cr);
            break;
        }
        case Channels::Overlaid:
            // Right first and darker, so the left -- the one most material
            // leads with -- is drawn over it in the true colour.
            paintGrid(cr, width, 0.0, height, false);
            paintBars(cr, width, right, 0.0, height, false, shade(barColor_, 160),
                      shade(peakColor_, 160));
            paintBars(cr, width, left, 0.0, height, false, barColor_, peakColor_);
            break;
    }
}

void SpectrumView::paintGrid(cairo_t* cr, double width, double top, double height, bool flipped) {
    // Positioned by the same normalisation the bars use, following the
    // configured floor rather than a fixed -80.
    const double floorDb = analyzers_[0].floorDb();
    setSource(cr, withAlpha({1.0F, 1.0F, 1.0F, 1.0F}, kGridAlpha));
    cairo_set_line_width(cr, 1.0);
    for (const int decibels : kGridLinesDb) {
        if (static_cast<double>(decibels) <= floorDb) {
            continue;
        }
        const double level = (static_cast<double>(decibels) - floorDb) / -floorDb;
        const double y =
            std::round(flipped ? top + (level * height) : top + height - (level * height)) + 0.5;
        cairo_move_to(cr, 0.0, y);
        cairo_line_to(cr, width, y);
    }
    cairo_stroke(cr);
}

void SpectrumView::paintBars(cairo_t* cr, double width, const SpectrumAnalyzer& analyzer,
                             double top, double height, bool flipped, const GdkRGBA& bar,
                             const GdkRGBA& peak) {
    const std::vector<float>& bands = analyzer.bands();
    const std::vector<float>& peaks = analyzer.peaks();
    if (bands.empty() || height <= 0.0) {
        return;
    }
    const double slot     = width / static_cast<double>(bands.size());
    const double gap      = slot / kGapDenominator;
    const double barWidth = std::max(1.0, slot - gap);

    const double base = flipped ? top : top + height;
    const double tip  = flipped ? top + height : top;

    // Base-to-tip gradient over the whole band rather than per bar, derived
    // from the chosen colour: darker at the base, lighter at the tip.
    cairo_pattern_t* gradient = cairo_pattern_create_linear(0.0, base, 0.0, tip);
    const GdkRGBA    dark     = shade(bar, 160);
    const GdkRGBA    light    = shade(bar, 80);
    cairo_pattern_add_color_stop_rgba(gradient, 0.0, dark.red, dark.green, dark.blue, dark.alpha);
    cairo_pattern_add_color_stop_rgba(gradient, 1.0, light.red, light.green, light.blue,
                                      light.alpha);
    cairo_set_source(cr, gradient);
    cairo_pattern_destroy(gradient);

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const double level = static_cast<double>(bands[band]);
        if (level <= 0.0) {
            continue;
        }
        const double x      = static_cast<double>(band) * slot;
        const double length = level * height;
        cairo_rectangle(cr, x, flipped ? base : base - length, barWidth, length);
    }
    cairo_fill(cr);

    if (!showPeaks_) {
        return;
    }
    // The peak markers last, over the bars, as one-pixel lines in their own
    // colour -- Cog's spectrumDotColor.
    setSource(cr, peak);
    cairo_set_line_width(cr, 1.0);
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const double held = static_cast<double>(peaks[band]);
        if (held <= 0.0) {
            continue;
        }
        const double x = static_cast<double>(band) * slot;
        const double y =
            std::round(flipped ? base + (held * height) : base - (held * height)) + 0.5;
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + barWidth, y);
    }
    cairo_stroke(cr);
}

// --- the oscilloscope --------------------------------------------------------------

OscilloscopeView::OscilloscopeView(AudioTap& tap, Settings& settings)
    : tap_(tap),
      settings_(settings),
      owned_(GObjectPtr<GtkWidget>::sink(gtk_drawing_area_new())),
      area_(owned_.get()),
      menu_(area_, "scope") {
    gtk_widget_set_size_request(area_, 200, 48);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area_),
        [](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<OscilloscopeView*>(data)->draw(cr, width, height);
        },
        this, nullptr);
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "map", [this](GtkWidget*) { setActive(playing_); }));
    connections_.push_back(Connection::to<void(GtkWidget*)>(
        area_, "unmap", [this](GtkWidget*) { timer_.stop(); }));
    buildMenu();
    applySettings(settings_);
}

OscilloscopeView::~OscilloscopeView() {
    timer_.stop();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
}

void OscilloscopeView::buildMenu() {
    GSimpleActionGroup* group = menu_.actions();
    addChoiceAction(group, "channels", connections_, [this](const char* key) {
        settings_.setScopeChannels(key);
        settingChanged.publish("scopeChannels");
    });
    addToggleAction(group, "trigger", connections_, [this] {
        settings_.setScopeTrigger(!settings_.ScopeTrigger());
        settingChanged.publish("scopeTrigger");
        return settings_.ScopeTrigger();
    });
    addToggleAction(group, "fill", connections_, [this] {
        settings_.setScopeFill(!settings_.ScopeFill());
        settingChanged.publish("scopeFill");
        return settings_.ScopeFill();
    });
    addToggleAction(group, "log-scale", connections_, [this] {
        settings_.setScopeLogScale(!settings_.ScopeLogScale());
        settingChanged.publish("scopeLogScale");
        return settings_.ScopeLogScale();
    });
    addPlainAction(group, "preferences", connections_, [this] { settingsRequested.publish(); });

    GMenu* model = menu_.model();
    GMenu* modes = g_menu_new();
    appendChoice(modes, tr("Mono"), "scope.channels", "mono");
    appendChoice(modes, tr("Left"), "scope.channels", "left");
    appendChoice(modes, tr("Right"), "scope.channels", "right");
    appendChoice(modes, tr("Stereo, stacked"), "scope.channels", "stacked");
    appendChoice(modes, tr("Stereo, overlaid"), "scope.channels", "overlaid");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(modes));
    g_object_unref(modes);
    GMenu* flags = g_menu_new();
    g_menu_append(flags, tr("Hold a steady tone still").c_str(), "scope.trigger");
    g_menu_append(flags, tr("Fill under the trace").c_str(), "scope.fill");
    g_menu_append(flags, tr("Logarithmic scale").c_str(), "scope.log-scale");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(flags));
    g_object_unref(flags);
    GMenu* rest = g_menu_new();
    g_menu_append(rest, preferencesLabel().c_str(), "scope.preferences");
    g_menu_append_section(model, nullptr, G_MENU_MODEL(rest));
    g_object_unref(rest);

    menu_.sync = [this, group] {
        setChoiceState(group, "channels", settings_.ScopeChannels().c_str());
        setToggleState(group, "trigger", settings_.ScopeTrigger());
        setToggleState(group, "fill", settings_.ScopeFill());
        setToggleState(group, "log-scale", settings_.ScopeLogScale());
    };
    menu_.build();
}

void OscilloscopeView::setSampleRate(double rate) {
    sampleRate_ = rate > 0.0 ? rate : 0.0;
    cursor_.setSampleRate(sampleRate_);
}

void OscilloscopeView::applySettings(const Settings& settings) {
    if (const auto colour = parseColour(settings.ScopeColor())) {
        colour_ = *colour;
    }
    if (const auto background = parseColour(settings.ScopeBackgroundColor())) {
        background_ = *background;
    }
    strokeWidth_ = std::clamp(settings.ScopeStrokeWidth(), 0.5, 6.0);
    gain_        = std::clamp(settings.ScopeGain(), 0.25, 8.0);
    windowMs_    = std::clamp(settings.ScopeWindowMs(), 5, 100);
    fill_        = settings.ScopeFill();
    trigger_     = settings.ScopeTrigger();
    logScale_    = settings.ScopeLogScale();
    channels_    = app::scopeChannelsFromKey(settings.ScopeChannels());

    const int frameRate = std::clamp(settings.ScopeFrameRate(), 15, 120);
    if (frameRate != frameRate_) {
        frameRate_ = frameRate;
        if (timer_.running()) {
            timer_.start(static_cast<unsigned>(std::max(1, 1000 / frameRate_)), [this] { tick(); });
        }
    }
    gtk_widget_queue_draw(area_);
}

std::size_t OscilloscopeView::windowFrames() const noexcept {
    if (sampleRate_ <= 0.0) {
        return 0;
    }
    return static_cast<std::size_t>(std::lround(sampleRate_ * windowMs_ / 1000.0));
}

std::size_t OscilloscopeView::fetchFrames() const noexcept {
    // Two windows: the trigger searches the older one for where the newer
    // should start. Bounded by what the tap holds behind a paced reader.
    const std::size_t granularity = tap_.writeGranularity();
    const std::size_t room        = tap_.capacity() > granularity + kTapMargin
                                        ? tap_.capacity() - granularity - kTapMargin
                                        : tap_.capacity() / 2;
    return std::min(2 * windowFrames(), room);
}

void OscilloscopeView::setActive(bool active) {
    playing_ = active;
    if (!active) {
        haveFrame_ = false;
        gtk_widget_queue_draw(area_);
    }
    if (active && gtk_widget_get_mapped(area_)) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.start(static_cast<unsigned>(std::max(1, 1000 / frameRate_)), [this] { tick(); });
    } else {
        timer_.stop();
    }
}

void OscilloscopeView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    const std::size_t fetch = fetchFrames();
    if (fetch < 2) {
        return;
    }
    mono_.resize(fetch);
    if (!cursor_.read(tap_, elapsed, mono_.data(), mono_.size(), TapLane::Mono)) {
        return;
    }
    if (channels_ != Channels::Mono) {
        left_.resize(fetch);
        right_.resize(fetch);
        if (!cursor_.readAgain(tap_, left_.data(), left_.size(), TapLane::Left) ||
            !cursor_.readAgain(tap_, right_.data(), right_.size(), TapLane::Right)) {
            return;
        }
    }
    // The window is the newer half; the trigger may pull its start back into
    // the older half. Run on the mix whichever channels are shown.
    const std::size_t window = std::min(windowFrames(), fetch / 2);
    traceFrames_             = window;
    traceStart_ = trigger_ ? triggerOffset(std::span<const float>{mono_}, window) : fetch - window;
    haveFrame_  = window > 0;
    gtk_widget_queue_draw(area_);
}

void OscilloscopeView::draw(cairo_t* cr, int width, int height) {
    setSource(cr, background_);
    cairo_paint(cr);
    if (width <= 0 || height <= 0) {
        return;
    }

    const std::size_t count = haveFrame_ ? traceFrames_ : 0;
    const auto        lane  = [&](const std::vector<float>& samples) -> const float* {
        return haveFrame_ ? samples.data() + traceStart_ : nullptr;
    };

    switch (channels_) {
        case Channels::Mono:
            paintTrace(cr, width, lane(mono_), count, 0, height, colour_);
            break;
        case Channels::Left:
            paintTrace(cr, width, lane(left_), count, 0, height, colour_);
            break;
        case Channels::Right:
            paintTrace(cr, width, lane(right_), count, 0, height, colour_);
            break;
        case Channels::Stacked: {
            const int half = height / 2;
            paintTrace(cr, width, lane(left_), count, 0, half, colour_);
            paintTrace(cr, width, lane(right_), count, half, height - half, colour_);
            break;
        }
        case Channels::Overlaid:
            paintTrace(cr, width, lane(right_), count, 0, height, shade(colour_, 160));
            paintTrace(cr, width, lane(left_), count, 0, height, colour_);
            break;
    }
}

void OscilloscopeView::paintTrace(cairo_t* cr, int width, const float* samples, std::size_t count,
                                  int top, int height, const GdkRGBA& colour) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const double centre    = top + (height / 2.0);
    const double amplitude = std::max(1.0, (height / 2.0) - kScopePadding);

    setSource(cr, withAlpha(colour, kCentreAlpha));
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0.0, std::round(centre) + 0.5);
    cairo_line_to(cr, width, std::round(centre) + 0.5);
    cairo_stroke(cr);

    if (samples == nullptr || count == 0) {
        return;
    }

    columns_.resize(static_cast<std::size_t>(width));
    foldForDisplay(std::span<const float>{samples, count}, static_cast<float>(gain_),
                   logScale_ ? ScopeScale::Logarithmic : ScopeScale::Linear, columns_);

    const auto   y      = [&](float value) { return centre - (static_cast<double>(value) * amplitude); };
    const double stroke = strokeWidth_;

    if (count <= columns_.size()) {
        // Sparse: at most one sample a column, so the trace is a polyline
        // through them, of the chosen width.
        const auto x = [](std::size_t column) { return static_cast<double>(column) + 0.5; };
        if (fill_) {
            cairo_new_path(cr);
            cairo_move_to(cr, x(0), centre);
            for (std::size_t column = 0; column < columns_.size(); ++column) {
                cairo_line_to(cr, x(column), y(columns_[column].second));
            }
            cairo_line_to(cr, x(columns_.size() - 1), centre);
            cairo_close_path(cr);
            setSource(cr, withAlpha(colour, kFillAlpha));
            cairo_fill(cr);
        }
        cairo_new_path(cr);
        cairo_move_to(cr, x(0), y(columns_[0].second));
        for (std::size_t column = 1; column < columns_.size(); ++column) {
            cairo_line_to(cr, x(column), y(columns_[column].second));
        }
        setSource(cr, colour);
        cairo_set_line_width(cr, stroke);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
        return;
    }

    // Dense: a column holds several samples, and the trace is the band from
    // each column's lowest to its highest, as one rectangle a column in one
    // fill -- OscilloscopePanel.cpp says why not a stroked outline.
    const double half = stroke / 2.0;
    if (fill_) {
        cairo_new_path(cr);
        for (std::size_t column = 0; column < columns_.size(); ++column) {
            double upper = y(columns_[column].second);
            double lower = y(columns_[column].first);
            if (column > 0) {
                upper = std::min(upper, y(columns_[column - 1].first));
                lower = std::max(lower, y(columns_[column - 1].second));
            }
            const double above = std::min(upper, centre);
            const double below = std::max(lower, centre);
            cairo_rectangle(cr, static_cast<double>(column), above, 1.0, centre - above);
            cairo_rectangle(cr, static_cast<double>(column), centre, 1.0, below - centre);
        }
        setSource(cr, withAlpha(colour, kFillAlpha));
        cairo_fill(cr);
    }
    cairo_new_path(cr);
    for (std::size_t column = 0; column < columns_.size(); ++column) {
        double upper = y(columns_[column].second);
        double lower = y(columns_[column].first);
        if (column > 0) {
            upper = std::min(upper, y(columns_[column - 1].first));
            lower = std::max(lower, y(columns_[column - 1].second));
        }
        cairo_rectangle(cr, static_cast<double>(column), upper - half, 1.0, (lower - upper) + stroke);
    }
    setSource(cr, colour);
    cairo_fill(cr);
}

}  // namespace xpcog::gtk
