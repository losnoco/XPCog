#include "Oscilloscope.hpp"

#include "Translations.hpp"

#include "xpcog/core/audio/Oscilloscope.hpp"

#include <winrt/Microsoft.Graphics.Canvas.Geometry.h>
#include <winrt/Windows.Foundation.Numerics.h>

#include <algorithm>
#include <cmath>
#include <span>

namespace xpcog::winui {

using app::tr;

namespace {

namespace canvas = winrt::Microsoft::Graphics::Canvas;
using winrt::Windows::Foundation::Numerics::float2;

// GTK's constants, unchanged; see app-gtk/src/Visualizers.cpp for each one's
// reason.
constexpr float       kScopePadding = 2.0F;
constexpr std::size_t kTapMargin    = 1024;
constexpr float       kFillAlpha    = 90.0F / 255.0F;
constexpr float       kCentreAlpha  = 70.0F / 255.0F;

/// The channel layouts, as the right-click menu offers them and the settings
/// key spells them. No "mirrored": a mirrored trace is the stacked one upside
/// down, and GTK's scope does not offer it either.
constexpr std::pair<const char*, const char*> kScopeLayouts[] = {
    {"mono", XPCOG_TRANSLATE("Mono")},
    {"left", XPCOG_TRANSLATE("Left")},
    {"right", XPCOG_TRANSLATE("Right")},
    {"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
    {"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
};

}  // namespace

OscilloscopeView::OscilloscopeView(AudioTap& tap, Settings& settings) : tap_(tap), settings_(settings) {
    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.MinHeight(48);
    canvas_.MinWidth(200);
    // Inset from the card's edges, as the spectrum is: a swap chain is not
    // clipped by a parent's corner radius.
    canvas_.Margin(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));
    canvas_.Draw([this](canvas::UI::Xaml::CanvasControl const& sender,
                        canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
             static_cast<float>(sender.ActualHeight()));
    });
    canvas_.ContextRequested([this](auto&&, mux::Input::ContextRequestedEventArgs const& args) {
        winrt::Windows::Foundation::Point at{};
        if (!args.TryGetPosition(canvas_, at)) {
            at = {8, 8};
        }
        showMenu(at);
        args.Handled(true);
    });
    canvas_.Unloaded([this](auto&&, auto&&) { timer_.Stop(); });
    canvas_.Loaded([this](auto&&, auto&&) { setActive(playing_); });

    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Tick([this](auto&&, auto&&) { tick(); });
    restartTimer();

    applySettings(settings_);
}

OscilloscopeView::~OscilloscopeView() {
    // After the XAML loop has ended, a call on the dispatcher queue throws,
    // and a throw out of a destructor is std::terminate -- see SpectrumView's.
    try {
        timer_.Stop();
    } catch (const winrt::hresult_error&) {
    }
}

void OscilloscopeView::restartTimer() {
    timer_.Interval(std::chrono::milliseconds(std::max(1, 1000 / frameRate_)));
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
        // Interval takes effect at the next tick; a running timer keeps going.
        restartTimer();
    }
    canvas_.ClearColor(background_);
    canvas_.Invalidate();
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
        canvas_.Invalidate();
    }
    if (active && canvas_.IsLoaded()) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        timer_.Start();
    } else {
        timer_.Stop();
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
    canvas_.Invalidate();
}

void OscilloscopeView::draw(const canvas::CanvasDrawingSession& ds, float width, float height) {
    // The background is the control's ClearColor; nothing to paint for it.
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }
    const int columns = static_cast<int>(width);

    const std::size_t count = haveFrame_ ? traceFrames_ : 0;
    const auto        lane  = [&](const std::vector<float>& samples) -> const float* {
        return haveFrame_ ? samples.data() + traceStart_ : nullptr;
    };

    switch (channels_) {
        case Channels::Mono:
            paintTrace(ds, columns, lane(mono_), count, 0.0F, height, colour_);
            break;
        case Channels::Left:
            paintTrace(ds, columns, lane(left_), count, 0.0F, height, colour_);
            break;
        case Channels::Right:
            paintTrace(ds, columns, lane(right_), count, 0.0F, height, colour_);
            break;
        case Channels::Stacked: {
            const float half = std::floor(height / 2.0F);
            paintTrace(ds, columns, lane(left_), count, 0.0F, half, colour_);
            paintTrace(ds, columns, lane(right_), count, half, height - half, colour_);
            break;
        }
        case Channels::Overlaid:
            paintTrace(ds, columns, lane(right_), count, 0.0F, height, shade(colour_, 160));
            paintTrace(ds, columns, lane(left_), count, 0.0F, height, colour_);
            break;
    }
}

void OscilloscopeView::paintTrace(const canvas::CanvasDrawingSession& ds, int width,
                                  const float* samples, std::size_t count, float top, float height,
                                  Colour colour) {
    if (width <= 0 || height <= 0.0F) {
        return;
    }
    const float centre    = top + (height / 2.0F);
    const float amplitude = std::max(1.0F, (height / 2.0F) - kScopePadding);
    const auto  w         = static_cast<float>(width);

    const float rule = std::round(centre) + 0.5F;
    ds.DrawLine(0.0F, rule, w, rule, withAlpha(colour, kCentreAlpha), 1.0F);

    if (samples == nullptr || count == 0) {
        return;
    }

    columns_.resize(static_cast<std::size_t>(width));
    foldForDisplay(std::span<const float>{samples, count}, static_cast<float>(gain_),
                   logScale_ ? ScopeScale::Logarithmic : ScopeScale::Linear, columns_);

    const auto  y      = [&](float value) { return centre - (value * amplitude); };
    const auto  stroke = static_cast<float>(strokeWidth_);

    if (count <= columns_.size()) {
        // Sparse: at most one sample a column, so the trace is a polyline
        // through them, of the chosen width.
        const auto x = [](std::size_t column) { return static_cast<float>(column) + 0.5F; };
        if (fill_) {
            auto path = canvas::Geometry::CanvasPathBuilder(ds);
            path.BeginFigure(float2{x(0), centre});
            for (std::size_t column = 0; column < columns_.size(); ++column) {
                path.AddLine(float2{x(column), y(columns_[column].second)});
            }
            path.AddLine(float2{x(columns_.size() - 1), centre});
            path.EndFigure(canvas::Geometry::CanvasFigureLoop::Closed);
            ds.FillGeometry(canvas::Geometry::CanvasGeometry::CreatePath(path),
                            withAlpha(colour, kFillAlpha));
        }
        auto path = canvas::Geometry::CanvasPathBuilder(ds);
        path.BeginFigure(float2{x(0), y(columns_[0].second)});
        for (std::size_t column = 1; column < columns_.size(); ++column) {
            path.AddLine(float2{x(column), y(columns_[column].second)});
        }
        path.EndFigure(canvas::Geometry::CanvasFigureLoop::Open);
        auto style = canvas::Geometry::CanvasStrokeStyle();
        style.LineJoin(canvas::Geometry::CanvasLineJoin::Round);
        ds.DrawGeometry(canvas::Geometry::CanvasGeometry::CreatePath(path), colour, stroke, style);
        return;
    }

    // Dense: a column holds several samples, and the trace is the band from
    // each column's lowest to its highest, a rectangle a column --
    // app/src/OscilloscopePanel.cpp says why not a stroked outline.
    const float half = stroke / 2.0F;
    if (fill_) {
        const Colour shaded = withAlpha(colour, kFillAlpha);
        for (std::size_t column = 0; column < columns_.size(); ++column) {
            float upper = y(columns_[column].second);
            float lower = y(columns_[column].first);
            if (column > 0) {
                upper = std::min(upper, y(columns_[column - 1].first));
                lower = std::max(lower, y(columns_[column - 1].second));
            }
            const float above = std::min(upper, centre);
            const float below = std::max(lower, centre);
            const auto  left  = static_cast<float>(column);
            ds.FillRectangle(left, above, 1.0F, centre - above, shaded);
            ds.FillRectangle(left, centre, 1.0F, below - centre, shaded);
        }
    }
    for (std::size_t column = 0; column < columns_.size(); ++column) {
        float upper = y(columns_[column].second);
        float lower = y(columns_[column].first);
        if (column > 0) {
            upper = std::min(upper, y(columns_[column - 1].first));
            lower = std::max(lower, y(columns_[column - 1].second));
        }
        ds.FillRectangle(static_cast<float>(column), upper - half, 1.0F, (lower - upper) + stroke,
                         colour);
    }
}

void OscilloscopeView::showMenu(winrt::Windows::Foundation::Point at) {
    auto menu = mux::Controls::MenuFlyout();
    const std::string current = settings_.ScopeChannels();
    for (const auto& [key, label] : kScopeLayouts) {
        auto item = mux::Controls::RadioMenuFlyoutItem();
        item.GroupName(L"layout");
        item.Text(toH(tr(label)));
        item.IsChecked(current == key);
        item.Click([this, key = std::string(key)](auto&&, auto&&) {
            settings_.setScopeChannels(key);
            settingChanged.publish("scopeChannels");
            applySettings(settings_);
        });
        menu.Items().Append(item);
    }
    menu.Items().Append(mux::Controls::MenuFlyoutSeparator());

    // The three toggles GTK's menu has, each a setting flipped and published.
    const auto toggle = [&](const char* label, bool on, auto flip) {
        auto item = mux::Controls::ToggleMenuFlyoutItem();
        item.Text(toH(tr(label)));
        item.IsChecked(on);
        item.Click([this, flip](auto&&, auto&&) {
            flip();
            applySettings(settings_);
        });
        menu.Items().Append(item);
    };
    toggle(XPCOG_TRANSLATE("Hold a steady tone still"), settings_.ScopeTrigger(), [this] {
        settings_.setScopeTrigger(!settings_.ScopeTrigger());
        settingChanged.publish("scopeTrigger");
    });
    toggle(XPCOG_TRANSLATE("Fill under the trace"), settings_.ScopeFill(), [this] {
        settings_.setScopeFill(!settings_.ScopeFill());
        settingChanged.publish("scopeFill");
    });
    toggle(XPCOG_TRANSLATE("Logarithmic scale"), settings_.ScopeLogScale(), [this] {
        settings_.setScopeLogScale(!settings_.ScopeLogScale());
        settingChanged.publish("scopeLogScale");
    });

    auto options = mux::Controls::Primitives::FlyoutShowOptions();
    options.Position(at);
    menu.ShowAt(canvas_, options);
}

}  // namespace xpcog::winui
