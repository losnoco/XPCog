// What Windows and Linux get until SMTC and MPRIS arrive in M5.
//
// A base-class instance rather than a null pointer, so the window connects to
// it unconditionally. Nothing reaches the OS and nothing has to check.

#include "xpcog/platform/MediaIntegration.hpp"

namespace xpcog::platform {

MediaIntegration* MediaIntegration::create(QObject* parent) {
    return new MediaIntegration(parent);
}

}  // namespace xpcog::platform
