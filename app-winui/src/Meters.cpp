#include "Meters.hpp"

#include "Commands.hpp"
#include "SignalChain.hpp"
#include "Translations.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace xpcog::winui {

namespace {

namespace canvas = winrt::Microsoft::Graphics::Canvas;

constexpr auto kFrameInterval = std::chrono::microseconds(1'000'000 / 30);

[[nodiscard]] float fraction(double decibels) {
    return static_cast<float>(
        std::clamp((decibels - LevelMeter::kFloor) / -LevelMeter::kFloor, 0.0, 1.0));
}

/// The oscilloscope's, for the views that share its strip.
void takeColours(const Settings& settings, canvas::UI::Xaml::CanvasControl& control,
                 Colour& colour) {
    colour = parseColour(settings.ScopeColor()).value_or(rgba(0.2F, 0.8F, 0.4F));
    control.ClearColor(parseColour(settings.ScopeBackgroundColor()).value_or(rgba(0, 0, 0)));
}

}  // namespace

// --- the VU meter ----------------------------------------------------------------

VuMeterView::VuMeterView(AudioTap& tap, const Settings& settings) : tap_(tap) {
    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.MinHeight(40);
    canvas_.MinWidth(200);
    canvas_.Margin(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));
    takeColours(settings, canvas_, colour_);
    canvas_.Draw([this](canvas::UI::Xaml::CanvasControl const& sender,
                        canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
             static_cast<float>(sender.ActualHeight()));
    });
    // A click clears the clip lamps, as Cog's does.
    canvas_.Tapped([this](auto&&, auto&&) {
        meter_.clearClip();
        canvas_.Invalidate();
    });
    canvas_.Unloaded([this](auto&&, auto&&) { ticker_.stop(); });
    canvas_.Loaded([this](auto&&, auto&&) { setActive(active_); });
    ticker_.setInterval(kFrameInterval);
}

void VuMeterView::setActive(bool active) {
    active_ = active;
    if (canvas_.IsLoaded()) {
        lastTick_ = std::chrono::steady_clock::now();
        ticker_.start();
    } else {
        ticker_.stop();
    }
}

void VuMeterView::tick() {
    const auto   now     = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_            = now;

    meter_.update(tap_, elapsed, sampleRate_ > 0.0 ? sampleRate_ : 44100.0, active_);
    canvas_.Invalidate();

    const bool atRest = meter_.left().hold <= LevelMeter::kFloor &&
                        meter_.right().hold <= LevelMeter::kFloor;
    if (!active_ && atRest) {
        ticker_.stop();
    }
}

void VuMeterView::draw(const canvas::CanvasDrawingSession& ds, float width, float height) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }

    constexpr float kGap  = 6.0F;
    constexpr float kLamp = 10.0F;
    const float     span  = std::max(1.0F, width - kLamp - kGap);
    const float     rowHeight = std::max(4.0F, (height - kGap) / 2.0F);

    const auto row = [&](int index, const LevelMeter::Channel& channel) {
        const float top = static_cast<float>(index) * (rowHeight + kGap);

        ds.FillRectangle(0.0F, top, span, rowHeight, withAlpha(colour_, 0.12F));
        ds.FillRectangle(0.0F, top, span * fraction(channel.peak), rowHeight,
                         withAlpha(colour_, 0.45F));
        ds.FillRectangle(0.0F, top, span * fraction(channel.rms), rowHeight, colour_);
        if (channel.hold > LevelMeter::kFloor) {
            ds.FillRectangle(span * fraction(channel.hold) - 1.0F, top, 2.0F, rowHeight, colour_);
        }
        ds.FillRectangle(width - kLamp, top, kLamp, rowHeight,
                         channel.clipped ? rgba(0.88F, 0.11F, 0.14F)
                                         : withAlpha(colour_, 0.12F));
    };

    row(0, meter_.left());
    row(1, meter_.right());
}

// --- the Waveform pane -------------------------------------------------------------

EnvelopeView::EnvelopeView(AudioTap& tap, const Settings& settings) : tap_(tap) {
    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.MinHeight(48);
    canvas_.MinWidth(200);
    canvas_.Margin(mux::ThicknessHelper::FromLengths(12, 4, 12, 12));
    takeColours(settings, canvas_, colour_);
    canvas_.Draw([this](canvas::UI::Xaml::CanvasControl const& sender,
                        canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
             static_cast<float>(sender.ActualHeight()));
    });
    canvas_.Unloaded([this](auto&&, auto&&) { ticker_.stop(); });
    canvas_.Loaded([this](auto&&, auto&&) { setActive(active_); });
    ticker_.setInterval(kFrameInterval);
}

void EnvelopeView::setSampleRate(double rate) {
    sampleRate_ = rate;
    cursor_.setSampleRate(rate);
    history_.setSampleRate(rate);
}

void EnvelopeView::setActive(bool active) {
    active_ = active;
    if (active && canvas_.IsLoaded()) {
        cursor_.reset();
        lastTick_ = std::chrono::steady_clock::now();
        ticker_.start();
    } else {
        ticker_.stop();
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
    canvas_.Invalidate();
}

void EnvelopeView::draw(const canvas::CanvasDrawingSession& ds, float width, float height) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }

    const float middle = height / 2.0F;
    ds.FillRectangle(0.0F, middle - 0.5F, width, 1.0F, withAlpha(colour_, 0.3F));

    const std::size_t columns = history_.size();
    const float       step    = width / static_cast<float>(columns);
    for (std::size_t i = 0; i < columns; ++i) {
        const EnvelopeHistory::Column& column = history_.at(i);
        const float top    = middle - std::clamp(column.high, -1.0F, 1.0F) * middle;
        const float bottom = middle - std::clamp(column.low, -1.0F, 1.0F) * middle;
        ds.FillRectangle(static_cast<float>(i) * step, top, std::max(step, 1.0F),
                         std::max(bottom - top, 1.0F), colour_);
    }
}

// --- the Signal Inspector ------------------------------------------------------------

SignalInspector::SignalInspector(app::Session& session) : session_(session) {
    window_ = mux::Window();
    window_.Title(toH(app::tr("Signal Inspector")));
    window_.SystemBackdrop(mux::Media::MicaBackdrop());

    auto column = mux::Controls::StackPanel();
    column.Spacing(12);
    column.Padding(mux::ThicknessHelper::FromLengths(16, 12, 16, 16));

    verdict_ = mux::Controls::TextBlock();
    verdict_.FontSize(20);
    column.Children().Append(verdict_);

    stages_ = mux::Controls::StackPanel();
    stages_.Spacing(6);
    column.Children().Append(stages_);

    meter_ = std::make_unique<VuMeterView>(session_.playback().tap(), session_.settings());
    meter_->element().as<mux::FrameworkElement>().Height(56);
    column.Children().Append(meter_->element());

    live_ = mux::Controls::TextBlock();
    live_.Opacity(0.7);
    column.Children().Append(live_);

    auto scroller = mux::Controls::ScrollViewer();
    scroller.Content(column);
    window_.Content(scroller);

    timer_ = window_.DispatcherQueue().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(250));
    timer_.Tick([this](auto&&, auto&&) { refresh(); });

    window_.AppWindow().Resize({420, 640});

    // The close button hides it, so the menu's tick can bring it back. Only the
    // destructor really closes it.
    window_.AppWindow().Closing(
        [this](auto&&, const winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs& args) {
            if (!closing_) {
                args.Cancel(true);
                hide();
            }
        });
}

SignalInspector::~SignalInspector() {
    closing_ = true;
    timer_.Stop();
    window_.Close();
}

void SignalInspector::toggle() {
    if (shown_) {
        hide();
    } else {
        show();
    }
}

void SignalInspector::show() {
    shown_ = true;
    window_.Activate();
    refresh();
    timer_.Start();
    if (visibilityChanged) {
        visibilityChanged();
    }
}

void SignalInspector::hide() {
    shown_ = false;
    timer_.Stop();
    meter_->setActive(false);
    window_.AppWindow().Hide();
    if (visibilityChanged) {
        visibilityChanged();
    }
}

void SignalInspector::refresh() {
    const SignalReport     report = session_.playback().signalReport();
    const app::SignalChain chain  = app::describeSignal(report);

    verdict_.Text(toH(chain.summary));
    meter_->setSampleRate(report.device.sampleRate);
    meter_->setActive(chain.active && !report.paused);

    std::string live;
    for (const std::string& line : chain.live) {
        live += (live.empty() ? "" : "\n") + line;
    }
    live_.Text(toH(live));

    // Rebuilt only when something changed: this runs four times a second.
    std::string key;
    for (const app::SignalStage& stage : chain.stages) {
        key += stage.name + '\n' + stage.detail + '\n' +
               std::to_string(static_cast<int>(stage.state)) + '\n';
    }
    if (key == shownStages_) {
        return;
    }
    shownStages_ = std::move(key);

    stages_.Children().Clear();
    for (const app::SignalStage& stage : chain.stages) {
        auto name = mux::Controls::TextBlock();
        name.Text(toH(stage.name));
        auto detail = mux::Controls::TextBlock();
        detail.Text(toH(stage.detail));
        detail.FontSize(12);
        detail.Opacity(0.7);
        detail.TextWrapping(mux::TextWrapping::Wrap);

        auto row = mux::Controls::StackPanel();
        row.Opacity(stage.state == app::SignalStage::State::Off ? 0.55 : 1.0);
        row.Children().Append(name);
        row.Children().Append(detail);
        stages_.Children().Append(row);
    }
}

}  // namespace xpcog::winui
