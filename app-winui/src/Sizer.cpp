#include "Sizer.hpp"

namespace xpcog::winui {

namespace {

namespace input = winrt::Microsoft::UI::Input;

/// A Grid that shows a cursor of its own. A class only for that:
/// ProtectedCursor is protected, so nothing outside a subclass can set it.
struct CursorSurface : mux::Controls::GridT<CursorSurface> {
    explicit CursorSurface(input::InputSystemCursorShape shape) {
        ProtectedCursor(input::InputSystemCursor::Create(shape));
    }
};

constexpr double kThickness = 8;   ///< the gap between cards, which this is
constexpr double kPillLength = 32;
constexpr double kPillWidth  = 4;

}  // namespace

Sizer::Sizer(Axis axis, std::function<double()> size, std::function<void(double)> resize,
             double sign)
    : axis_(axis), size_(std::move(size)), resize_(std::move(resize)), sign_(sign) {
    const bool columns = axis_ == Axis::Columns;
    root_ = winrt::make<CursorSurface>(columns ? input::InputSystemCursorShape::SizeWestEast
                                               : input::InputSystemCursorShape::SizeNorthSouth);
    // Transparent rather than null: a null background is not hit-testable,
    // and the whole gap is the handle, not only the pill.
    root_.Background(mux::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
    if (columns) {
        root_.Width(kThickness);
    } else {
        root_.Height(kThickness);
    }

    pill_ = loadXaml<mux::Controls::Border>(
        L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
        L" CornerRadius='2' Background='{ThemeResource ControlStrongFillColorDefaultBrush}'"
        L" HorizontalAlignment='Center' VerticalAlignment='Center' Opacity='0'/>");
    pill_.Width(columns ? kPillWidth : kPillLength);
    pill_.Height(columns ? kPillLength : kPillWidth);
    root_.Children().Append(pill_);

    root_.PointerEntered([this](auto&&, auto&&) { pill_.Opacity(1); });
    root_.PointerExited([this](auto&&, auto&&) {
        if (!dragging_) {
            pill_.Opacity(0);
        }
    });

    // Positions against the window, not the handle: the handle moves as the
    // pane it resizes does, and measured against itself the drag would chase
    // its own tail.
    const auto along = [this](mux::Input::PointerRoutedEventArgs const& args) {
        const auto point = args.GetCurrentPoint(nullptr).Position();
        return axis_ == Axis::Columns ? point.X : point.Y;
    };
    root_.PointerPressed([this, along](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
        if (!args.GetCurrentPoint(root_).Properties().IsLeftButtonPressed()) {
            return;
        }
        dragging_  = true;
        startAt_   = along(args);
        startSize_ = size_();
        root_.CapturePointer(args.Pointer());
        args.Handled(true);
    });
    root_.PointerMoved([this, along](auto&&, mux::Input::PointerRoutedEventArgs const& args) {
        if (dragging_) {
            resize_(startSize_ + sign_ * (along(args) - startAt_));
            args.Handled(true);
        }
    });
    const auto end = [this](auto&&, auto&&) {
        if (!dragging_) {
            return;
        }
        dragging_ = false;
        pill_.Opacity(0);
        if (finished) {
            finished();
        }
    };
    root_.PointerReleased([this, end](auto&& sender, mux::Input::PointerRoutedEventArgs const& args) {
        root_.ReleasePointerCapture(args.Pointer());
        end(sender, args);
    });
    root_.PointerCaptureLost(end);
}

}  // namespace xpcog::winui
