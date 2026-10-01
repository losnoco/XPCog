#include "Runtime.hpp"

#include <windows.h>

#include <MddBootstrap.h>

#include <cstdio>

namespace xpcog::winui {

namespace {

bool bootstrapped = false;

}  // namespace

bool bootstrapRuntime(std::string& reason) {
    // From 2.0 only the major version is matched; the minimum then picks any
    // 2.x runtime at or above the one the projection was generated from.
    PACKAGE_VERSION minimum{};
    minimum.Major    = XPCOG_WINAPPSDK_RUNTIME_MAJOR;
    minimum.Minor    = XPCOG_WINAPPSDK_RUNTIME_MINOR;
    minimum.Build    = XPCOG_WINAPPSDK_RUNTIME_PATCH;
    minimum.Revision = 0;
    const HRESULT hr = MddBootstrapInitialize2(
        (XPCOG_WINAPPSDK_RUNTIME_MAJOR << 16) | XPCOG_WINAPPSDK_RUNTIME_MINOR, L"", minimum,
        MddBootstrapInitializeOptions_None);
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "XPCog needs the Windows App Runtime %d.%d.%d or later, and it was not "
                      "found (0x%08lX).",
                      XPCOG_WINAPPSDK_RUNTIME_MAJOR, XPCOG_WINAPPSDK_RUNTIME_MINOR,
                      XPCOG_WINAPPSDK_RUNTIME_PATCH, static_cast<unsigned long>(hr));
        reason = text;
        return false;
    }
    bootstrapped = true;
    return true;
}

void shutdownRuntime() {
    if (bootstrapped) {
        MddBootstrapShutdown();
        bootstrapped = false;
    }
}

}  // namespace xpcog::winui
