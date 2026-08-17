// Everywhere without a taskbar-button implementation.
//
// That currently means macOS and Linux, for different reasons. macOS has the
// surface -- NSDockTile, which is what Cog's DockIconController draws on -- and
// simply is not written yet. Linux has no single answer: the Unity launcher API
// that a few desktops still honour is a D-Bus signal, and on the rest there is
// nowhere to put a badge at all.
//
// A base-class instance rather than a null pointer, so the window wires it up
// unconditionally and nothing has to check.

#include "xpcog/platform/TaskbarIntegration.hpp"

namespace xpcog::platform {

TaskbarIntegration* TaskbarIntegration::create(WId window, QObject* parent) {
    (void)window;
    return new TaskbarIntegration(parent);
}

}  // namespace xpcog::platform
