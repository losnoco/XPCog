// No StatusNotifierItem here, which is the right answer on two of the three
// platforms rather than a gap.
//
// Windows has a notification area that wxTaskBarIcon drives correctly, and macOS
// deliberately has no tray at all -- see StatusPresence.hpp for why the Dock menu
// is the platform's answer instead. Both get this, report themselves unavailable,
// and the application falls through to the wx path it has always used.
//
// The base class already does nothing; what this file supplies is the factory, so
// that the seam resolves at link time on every platform and the application needs
// no #ifdef of its own. Same shape as NullMediaIntegration.cpp next door.

#include "xpcog/platform/TrayIcon.hpp"

#include <memory>
#include <utility>

namespace xpcog::platform {

std::unique_ptr<TrayIcon> TrayIcon::create(Dispatcher dispatch) {
    return std::make_unique<TrayIcon>(std::move(dispatch));
}

}  // namespace xpcog::platform
