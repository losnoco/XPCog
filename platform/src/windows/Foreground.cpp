#include "xpcog/platform/Foreground.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
// Otherwise windows.h defines min and max as macros, which breaks every
// std::min / std::max in any translation unit that ends up including this.
#define NOMINMAX
#endif
#include <windows.h>

namespace xpcog::platform {

void permitForegroundHandover() {
    // ASFW_ANY rather than a specific process id, because the id is not in reach:
    // the receiver is on the far end of an inter-process channel whose underlying
    // pipe handle no toolkit exposes, and GetNamedPipeServerProcessId would need
    // exactly that. Adding a
    // reply to the protocol purely to learn it would buy very little -- this
    // permission lives only until the next foreground change and belongs to a
    // process that is about to exit, so the window in which "any" is broader than
    // "that one" is measured in milliseconds of a process with no windows.
    //
    // The return value is deliberately ignored. It fails precisely when this
    // process was not entitled to the foreground in the first place, in which case
    // there was nothing to give away and nothing to do about it.
    static_cast<void>(AllowSetForegroundWindow(ASFW_ANY));
}

}  // namespace xpcog::platform
