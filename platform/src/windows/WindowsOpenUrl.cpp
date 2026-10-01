#include "xpcog/platform/OpenUrl.hpp"

#include "WinString.hpp"

#include <windows.h>
#include <shellapi.h>

namespace xpcog::platform {

void openInBrowser(const std::string& url) {
    // ShellExecuteW rather than a CreateProcess on a browser: the association is
    // the user's and the scheme may not be http at all.
    ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace xpcog::platform
