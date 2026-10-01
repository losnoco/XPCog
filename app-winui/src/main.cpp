#include "App.hpp"
#include "Runtime.hpp"
#include "Win2D.hpp"
#include "WinRT.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // XAML wants a single-threaded apartment on the interface thread.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    // Before anything Win2D is touched; see Win2D.hpp.
    xpcog::winui::installWin2DActivation();

    std::string reason;
    if (!xpcog::winui::bootstrapRuntime(reason)) {
        // No WinUI to say it with, so the oldest dialog there is.
        ::MessageBoxW(nullptr, winrt::to_hstring(reason).c_str(), L"XPCog",
                      MB_OK | MB_ICONERROR);
        return 1;
    }

    const int status = xpcog::winui::runApplication();
    xpcog::winui::shutdownRuntime();
    return status;
}
