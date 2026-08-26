// Everywhere with no desktop to ask.
//
// Both calls report that nothing happened, which is what the caller is written
// to expect: the menu items are disabled where they cannot work, and the status
// line says so if one is reached anyway. A file rather than an #ifdef inside one
// of the real implementations, to match how every other seam here is chosen --
// the choice is made in CMakeLists.txt, where it can be read.

#include "xpcog/platform/FileManager.hpp"

namespace xpcog::platform {

bool revealInFileManager(const std::filesystem::path&) { return false; }

bool moveToTrash(const std::filesystem::path&) { return false; }

}  // namespace xpcog::platform
