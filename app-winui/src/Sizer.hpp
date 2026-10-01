#pragma once

#include "WinRT.hpp"

#include <functional>

namespace xpcog::winui {

/// A drag handle in the gap between two panes, which resizes one of them.
///
/// WinUI has no splitter -- the Community Toolkit's GridSplitter is C# only --
/// so this is the gap itself made draggable: the resize cursor over it, a
/// pill that shows on hover the way Windows 11's own pane dividers do, and a
/// size that follows the pointer while it is held.
class Sizer {
public:
    enum class Axis {
        Columns,  ///< a vertical gap; dragging sideways changes a width
        Rows,     ///< a horizontal gap; dragging up and down changes a height
    };

    /// `size` reads the pane's current size; `resize` applies one, clamped as
    /// the pane needs. `sign` is +1 when dragging right or down makes the pane
    /// bigger -- a pane left of or above the gap -- and -1 for one after it.
    Sizer(Axis axis, std::function<double()> size, std::function<void(double)> resize, double sign);

    [[nodiscard]] mux::FrameworkElement element() const { return root_; }

    /// A drag ended: the size is worth remembering now.
    std::function<void()> finished;

private:
    Axis                            axis_;
    std::function<double()>         size_;
    std::function<void(double)>     resize_;
    double                          sign_;
    mux::Controls::Grid             root_{nullptr};
    mux::Controls::Border           pill_{nullptr};
    bool                            dragging_  = false;
    double                          startAt_   = 0;
    double                          startSize_ = 0;
};

}  // namespace xpcog::winui
