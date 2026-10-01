#include "SeekBar.hpp"

#include "TrackText.hpp"
#include "Translations.hpp"

#include <winrt/Microsoft.Graphics.Canvas.Geometry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace xpcog::winui {

namespace canvas = winrt::Microsoft::Graphics::Canvas;

namespace {

// The geometry is the wx and GTK bars', in DIPs.
constexpr float kGrooveHeight = 4.0F;
constexpr float kThumbRadius  = 6.0F;
constexpr float kMargin       = kThumbRadius + 1.0F;

constexpr float  kWaveformPadding = 2.0F;
constexpr float  kPlayheadWidth   = 2.0F;
/// The bottom of the logarithmic scale. One step of the byte a bucket is
/// stored in is 20*log10(1/255) = -48.1 dB, so a floor there is where the
/// stored resolution runs out: the quietest non-zero bucket is drawn at the
/// bottom of the scale rather than a fifth of the way up it, which a deeper
/// floor would do, and silence and near-silence still tell apart.
constexpr double kLogFloorDb = -48.0;

constexpr float kPeakAlpha          = 55.0F / 255.0F;
constexpr float kRmsAlpha           = 120.0F / 255.0F;
constexpr float kGrooveOutlineAlpha = 90.0F / 255.0F;
constexpr float kThumbOutlineAlpha  = 210.0F / 255.0F;
/// The unfilled groove: the text colour, faint, as GTK has it.
constexpr float kGrooveAlpha = 0.22F;

/// The slider's thumb tooltip: the position as a clock rather than as a bare
/// number of seconds.
struct ClockConverter : winrt::implements<ClockConverter, mux::Data::IValueConverter> {
    winrt::Windows::Foundation::IInspectable Convert(
        winrt::Windows::Foundation::IInspectable const& value,
        winrt::Windows::UI::Xaml::Interop::TypeName const&,
        winrt::Windows::Foundation::IInspectable const&, winrt::hstring const&) const {
        return winrt::box_value(toH(app::formatClock(winrt::unbox_value_or<double>(value, 0.0))));
    }
    winrt::Windows::Foundation::IInspectable ConvertBack(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Windows::UI::Xaml::Interop::TypeName const&,
        winrt::Windows::Foundation::IInspectable const&, winrt::hstring const&) const {
        throw winrt::hresult_not_implemented();
    }
};

[[nodiscard]] bool same(const std::optional<Colour>& a, const std::optional<Colour>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a || (a->A == b->A && a->R == b->R && a->G == b->G && a->B == b->B);
}

}  // namespace

SeekBar::SeekBar() : self_(std::make_shared<SeekBar*>(this)) {
    root_ = mux::Controls::Grid();
    root_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    root_.VerticalAlignment(mux::VerticalAlignment::Center);

    // --- the slider -------------------------------------------------------------
    slider_ = mux::Controls::Slider();
    slider_.Minimum(0.0);
    slider_.Maximum(1.0);
    slider_.StepFrequency(0.1);
    // Arrow keys step five seconds and Page keys thirty, which is what a
    // listener reaching for the keyboard wants rather than a percentage.
    slider_.SmallChange(5.0);
    slider_.LargeChange(30.0);
    slider_.MinWidth(120);
    slider_.VerticalAlignment(mux::VerticalAlignment::Center);
    slider_.ThumbToolTipValueConverter(winrt::make<ClockConverter>());
    mux::Automation::AutomationProperties::SetName(slider_, toH(app::tr("Position")));

    slider_.ValueChanged([this](auto&&, mux::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
        if (syncing_ || duration_ <= 0.0) {
            return;
        }
        position_ = std::clamp(args.NewValue(), 0.0, duration_);
        if (pressed_) {
            // A drag: follow it on the clock, and seek when it lets go.
            scrubbing_ = true;
            scrubbed.publish(position_);
        } else {
            // The keyboard, a wheel, or the click that began a press: one
            // step, one seek.
            seekRequested.publish(position_);
        }
    });

    // The Slider handles its own pointer events, so these listen with
    // handledEventsToo to hear about them at all.
    slider_.AddHandler(mux::UIElement::PointerPressedEvent(),
                       winrt::box_value(mux::Input::PointerEventHandler([this](auto&&, auto&&) {
                           pressed_   = true;
                           scrubbing_ = false;
                       })),
                       true);
    // Release and capture loss both end the press; whichever comes first does
    // the work and the other finds nothing to do. On release, not on every
    // motion: a seek is a decoder reopen for some formats, and a drag across a
    // long track would ask the engine for a hundred.
    const auto release = [this](auto&&, auto&&) {
        if (!pressed_) {
            return;
        }
        pressed_        = false;
        const bool seek = scrubbing_;
        scrubbing_      = false;
        if (seek) {
            seekRequested.publish(position_);
        }
    };
    slider_.AddHandler(mux::UIElement::PointerReleasedEvent(),
                       winrt::box_value(mux::Input::PointerEventHandler(release)), true);
    slider_.AddHandler(mux::UIElement::PointerCaptureLostEvent(),
                       winrt::box_value(mux::Input::PointerEventHandler(release)), true);
    root_.Children().Append(slider_);

    // --- the waveform -------------------------------------------------------------
    canvas_ = canvas::UI::Xaml::CanvasControl();
    canvas_.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    canvas_.MinWidth(120);
    canvas_.Visibility(mux::Visibility::Collapsed);
    // Transparent, so the strip sits on the window's Mica like the slider
    // does rather than on a box of its own.
    canvas_.ClearColor(Colour{0, 0, 0, 0});
    canvas_.Draw([weak = std::weak_ptr<SeekBar*>(self_)](
                     canvas::UI::Xaml::CanvasControl const& sender,
                     canvas::UI::Xaml::CanvasDrawEventArgs const& args) {
        if (const auto self = weak.lock(); self && *self) {
            (*self)->draw(args.DrawingSession(), static_cast<float>(sender.ActualWidth()),
                          static_cast<float>(sender.ActualHeight()));
        }
    });

    canvas_.PointerPressed([this](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
        const auto point = args.GetCurrentPoint(canvas_);
        if (duration_ <= 0.0 || !point.Properties().IsLeftButtonPressed()) {
            return;
        }
        canvas_.CapturePointer(args.Pointer());
        scrubbing_ = true;
        // Straight to where the click landed, rather than paging towards it.
        // This is the behaviour the whole widget exists for.
        position_ = positionAt(point.Position().X);
        scrubbed.publish(position_);
        invalidate();
        args.Handled(true);
    });
    canvas_.PointerMoved([this](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
        if (!scrubbing_) {
            return;
        }
        position_ = positionAt(args.GetCurrentPoint(canvas_).Position().X);
        scrubbed.publish(position_);
        invalidate();
        args.Handled(true);
    });
    canvas_.PointerReleased([this](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
        if (!scrubbing_) {
            return;
        }
        position_  = positionAt(args.GetCurrentPoint(canvas_).Position().X);
        scrubbing_ = false;
        canvas_.ReleasePointerCapture(args.Pointer());
        seekRequested.publish(position_);
        invalidate();
        args.Handled(true);
    });
    canvas_.PointerCaptureLost([this](auto&&, auto&&) {
        // Lost without a release -- a dialog took the pointer -- is an
        // interrupted gesture, not a completed one, and seeks nothing.
        if (scrubbing_) {
            scrubbing_ = false;
            invalidate();
        }
    });
    canvas_.SizeChanged([this](auto&&, auto&&) { invalidate(); });
    root_.Children().Append(canvas_);

    // Every colour this draws is read at paint time, so a repaint is the whole
    // of what an appearance change needs -- but something has to ask for it,
    // and a bar sitting at 0:00 has no other reason to redraw. The accent
    // change arrives on a thread of its own.
    canvas_.ActualThemeChanged([this](auto&&, auto&&) { invalidate(); });
    colourToken_ = uiSettings_.ColorValuesChanged(
        [weak  = std::weak_ptr<SeekBar*>(self_),
         queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()](
            auto&&, auto&&) {
            queue.TryEnqueue([weak] {
                if (const auto self = weak.lock(); self && *self) {
                    (*self)->invalidate();
                }
            });
        });

    syncSlider();
    choose();
}

SeekBar::~SeekBar() {
    // Runs after the XAML loop has ended, when a call into XAML throws and a
    // throw out of a destructor is std::terminate. So: no XAML here. The weak
    // pointers the callbacks hold go dead with self_, and a late one finds
    // nothing.
    *self_ = nullptr;
    self_.reset();
    try {
        uiSettings_.ColorValuesChanged(colourToken_);
    } catch (const winrt::hresult_error&) {
    }
}

void SeekBar::invalidate() {
    if (canvas_ && canvas_.Visibility() == mux::Visibility::Visible) {
        canvas_.Invalidate();
    }
}

void SeekBar::syncSlider() {
    // Set, never reported as a change: syncing_ is what tells the slider's
    // ValueChanged that the code moved it and the listener did not.
    syncing_ = true;
    slider_.IsEnabled(duration_ > 0.0);
    slider_.Maximum(duration_ > 0.0 ? duration_ : 1.0);
    slider_.Value(duration_ > 0.0 ? position_ : 0.0);
    syncing_ = false;
}

void SeekBar::choose() {
    const bool painted =
        waveformMode_ && waveform_ && duration_ > 0.0 && waveform_->bucketCount > 0;
    slider_.Visibility(painted ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    canvas_.Visibility(painted ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    invalidate();
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
    syncSlider();
    choose();
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
        syncing_ = true;
        slider_.Value(position_);
        syncing_ = false;
    }
    invalidate();
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
    const double height = static_cast<double>(style_.height);
    canvas_.Height(height);
    root_.Height(on ? height : std::numeric_limits<double>::quiet_NaN());
    choose();
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
            setWaveformMode(true);  // re-applies the height
        }
        invalidate();
    }
}

void SeekBar::setWaveform(std::shared_ptr<const WaveformSummary> summary) {
    waveform_ = std::move(summary);
    choose();
}

double SeekBar::positionAt(double x) const {
    const double width = canvas_.ActualWidth() - (2.0 * kMargin);
    if (width <= 0.0 || duration_ <= 0.0) {
        return 0.0;
    }
    const double fraction = std::clamp((x - kMargin) / width, 0.0, 1.0);
    return fraction * duration_;
}

float SeekBar::thumbCentre() const {
    const auto width = static_cast<float>(canvas_.ActualWidth()) - (2.0F * kMargin);
    if (width <= 0.0F) {
        return kMargin;
    }
    const auto fraction = static_cast<float>(duration_ > 0.0 ? position_ / duration_ : 0.0);
    return kMargin + std::floor(fraction * width);
}

Colour SeekBar::foreground() const {
    // TextFillColorPrimary's two values, chosen by the theme the bar is
    // actually drawn in -- which follows a light/dark switch, where the
    // application-level resource lookup stays on the theme it launched with.
    return canvas_.ActualTheme() == mux::ElementTheme::Dark ? Colour{255, 255, 255, 255}
                                                            : Colour{0xE4, 0, 0, 0};
}

Colour SeekBar::accent() const {
    // AccentFillColorDefault's derivation: the system accent, a step darker on
    // a light theme and two lighter on a dark one, so it reads either way.
    using winrt::Windows::UI::ViewManagement::UIColorType;
    try {
        return uiSettings_.GetColorValue(canvas_.ActualTheme() == mux::ElementTheme::Dark
                                             ? UIColorType::AccentLight2
                                             : UIColorType::AccentDark1);
    } catch (const winrt::hresult_error&) {
        return Colour{255, 0, 95, 184};  // WinUI's own fallback blue
    }
}

void SeekBar::draw(const canvas::CanvasDrawingSession& ds, float width, float height) {
    // Shown only with a shape to draw; choose() puts the slider up otherwise.
    if (!waveform_ || duration_ <= 0.0 || waveform_->bucketCount == 0) {
        return;
    }
    const float centreY    = height / 2.0F;
    const float span       = std::max(0.0F, width - (2.0F * kMargin));
    const float halfHeight = std::max(1.0F, centreY - kWaveformPadding);
    paintWaveform(ds, kMargin, span, centreY, halfHeight);
}

void SeekBar::paintWaveform(const canvas::CanvasDrawingSession& ds, float left, float width,
                            float centreY, float halfHeight) {
    const WaveformSummary& shape   = *waveform_;
    const auto             columns = static_cast<int>(width);
    if (columns <= 0) {
        return;
    }

    const Colour fg           = foreground();
    const Colour trackColour  = withAlpha(fg, kGrooveAlpha);
    const Colour accentColour = style_.played.value_or(accent());
    const Colour playedPeak   = withAlpha(accentColour, kPeakAlpha + (60.0F / 255.0F));
    const Colour playedRms    = accentColour;
    constexpr float hairline  = 1.0F;

    // The unplayed part: a shade of the text colour unless a colour was
    // chosen, because the text colour is the one thing guaranteed to read
    // against the window in either appearance.
    const auto unplayed = [&](float alpha) { return withAlpha(style_.unplayed.value_or(fg), alpha); };

    // Where the shape stops and the plain groove begins, in columns. A bucket
    // is drawn once every column it covers has been analysed.
    const double bucketsPerColumn = static_cast<double>(shape.bucketCount) / columns;
    const int    analysedColumns =
        std::clamp(static_cast<int>(std::floor(shape.analysed / bucketsPerColumn)), 0, columns);
    const int playedColumns = std::clamp(static_cast<int>(thumbCentre() - left), 0, columns);

    const bool  rectified = style_.rectified;
    const float baseline  = rectified ? centreY + halfHeight : centreY;
    const float amplitude = rectified ? 2.0F * halfHeight : halfHeight;

    // The baseline first, under everything, so a silent stretch still reads as
    // part of the bar rather than a gap in it.
    ds.DrawLine(left, baseline + 0.5F, left + width, baseline + 0.5F, trackColour, hairline);

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
        return static_cast<float>(scale(peak)) * amplitude;
    };

    // One polygon per (level, played) pair: across the top edge of every
    // column, then back along the bottom, filled in one go so there are no
    // seams between columns.
    const auto fill = [&](const std::vector<std::uint8_t>& values, int from, int to, Colour colour) {
        if (to <= from) {
            return;
        }
        auto path = canvas::Geometry::CanvasPathBuilder(ds);
        path.BeginFigure(left + static_cast<float>(from), baseline);
        for (int x = from; x < to; ++x) {
            const float h = level(values, x);
            path.AddLine(left + static_cast<float>(x), baseline - h);
            path.AddLine(left + static_cast<float>(x + 1), baseline - h);
        }
        path.AddLine(left + static_cast<float>(to), baseline);
        if (!rectified) {
            for (int x = to - 1; x >= from; --x) {
                const float h = level(values, x);
                path.AddLine(left + static_cast<float>(x + 1), baseline + h);
                path.AddLine(left + static_cast<float>(x), baseline + h);
            }
        }
        path.EndFigure(canvas::Geometry::CanvasFigureLoop::Closed);
        ds.FillGeometry(canvas::Geometry::CanvasGeometry::CreatePath(path), colour);
    };

    const int split = std::min(playedColumns, analysedColumns);
    fill(shape.peak, 0, split, playedPeak);
    fill(shape.peak, split, analysedColumns, unplayed(kPeakAlpha));
    fill(shape.rms, 0, split, playedRms);
    fill(shape.rms, split, analysedColumns, unplayed(kRmsAlpha));

    // Past the analysis: the plain groove, with the played part filled if the
    // playhead has got there first.
    if (analysedColumns < columns) {
        const float top    = rectified ? baseline - kGrooveHeight : centreY - (kGrooveHeight / 2.0F);
        const float x      = left + static_cast<float>(analysedColumns);
        const float rest   = width - static_cast<float>(analysedColumns);
        const float radius = kGrooveHeight / 2.0F;
        const float w      = std::max(0.0F, rest - hairline);
        const float h      = std::max(0.0F, kGrooveHeight - hairline);
        if (w > 0.0F && h > 0.0F) {
            ds.FillRoundedRectangle(x + (hairline / 2.0F), top + (hairline / 2.0F), w, h, radius,
                                    radius, trackColour);
            ds.DrawRoundedRectangle(x + (hairline / 2.0F), top + (hairline / 2.0F), w, h, radius,
                                    radius, withAlpha(fg, kGrooveOutlineAlpha), hairline);
        }
        if (playedColumns > analysedColumns) {
            const float played =
                std::max(0.0F, static_cast<float>(playedColumns - analysedColumns) - hairline);
            if (played > 0.0F && h > 0.0F) {
                ds.FillRoundedRectangle(x + (hairline / 2.0F), top + (hairline / 2.0F), played, h,
                                        radius, radius, accentColour);
            }
        }
    }

    // The playhead, full height, in the foreground colour: it has to read over
    // the accent on its left and the grey on its right.
    ds.FillRectangle(thumbCentre() - (kPlayheadWidth / 2.0F), centreY - halfHeight, kPlayheadWidth,
                     2.0F * halfHeight, withAlpha(fg, kThumbOutlineAlpha));
}

}  // namespace xpcog::winui
