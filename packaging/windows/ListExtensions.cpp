// Prints every extension this build's decoders claim, one per line: the list
// the installer offers XPCog for in the "Open with" lists.
//
// Run at packaging time by Associations.cmake. It links the same xpcog-codecs
// the player does and asks the same registry the player's open dialog asks,
// so the installer's list is this build's and not one written down beside it
// -- which would be wrong the first time a decoder was added, and quietly.

#include "xpcog/core/PluginRegistry.hpp"

#include <cstdio>
#include <string>

int main() {
    xpcog::PluginRegistry registry;
    xpcog::registerAllCodecs(registry);
    for (const std::string& extension : registry.allExtensions()) {
        std::printf("%s\n", extension.c_str());
    }
    return 0;
}
