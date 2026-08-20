// What a platform with no media-key or now-playing surface gets.
//
// A base-class instance rather than a null pointer, so the window wires up
// unconditionally. Nothing reaches the OS and nothing has to check.

#include "xpcog/platform/MediaIntegration.hpp"

#include <utility>

namespace xpcog::platform {

std::unique_ptr<MediaIntegration> MediaIntegration::create(Dispatcher dispatch, void* nativeWindow) {
    (void)nativeWindow;
    return std::make_unique<MediaIntegration>(std::move(dispatch));
}

}  // namespace xpcog::platform
