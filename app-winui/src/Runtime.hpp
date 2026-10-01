#pragma once

#include <string>

namespace xpcog::winui {

/// Makes the Windows App Runtime framework package available to this
/// unpackaged process -- the step a packaged app gets from its manifest. Has
/// to run before the first WinRT type from Microsoft.UI is touched.
///
/// False when the runtime is not installed, or is older than the one these
/// headers were generated from; `reason` then says so, for a message box, since
/// without the runtime there is no WinUI to say it with.
bool bootstrapRuntime(std::string& reason);

/// Undoes bootstrapRuntime(). Last thing before the process exits.
void shutdownRuntime();

}  // namespace xpcog::winui
