#include "xpcog/platform/OpenUrl.hpp"

#include <gio/gio.h>

namespace xpcog::platform {

void openInBrowser(const std::string& url) {
    // The same call GioFileManager uses to reveal a file: gio asks the desktop's
    // own handler rather than this program guessing at xdg-open, which is not
    // present everywhere and is the wrong answer inside a Flatpak.
    GError* error = nullptr;
    g_app_info_launch_default_for_uri(url.c_str(), nullptr, &error);
    g_clear_error(&error);
}

}  // namespace xpcog::platform
