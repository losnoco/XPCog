// Everywhere that is not Windows.
//
// Nothing to do: macOS and Linux let an application activate its own window, so
// the receiving side's raise is enough on its own. A file rather than an #ifdef
// inside the Windows one, to match how MediaIntegration is selected -- the choice
// of implementation is made in CMakeLists.txt, where it can be read.

#include "xpcog/platform/Foreground.hpp"

namespace xpcog::platform {

void permitForegroundHandover() {}

}  // namespace xpcog::platform
