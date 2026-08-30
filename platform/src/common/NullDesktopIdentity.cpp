// Windows and macOS: there is no desktop application ID in this sense.
//
// Both have an application identity, and both already carry one -- the macOS
// bundle identifier in Info.plist, the Windows AppUserModelID discussed in
// docs/PORTING.md -- but neither is a string the process announces to a shell
// at startup, and neither is read from here. See the header.

#include "xpcog/platform/DesktopIdentity.hpp"

namespace xpcog::platform {

std::string_view desktopApplicationId() { return {}; }

void applyDesktopIdentity() {}

}  // namespace xpcog::platform
