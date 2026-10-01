#include "Instance.hpp"

#include "WinRT.hpp"

#include "xpcog/platform/Foreground.hpp"

#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include <combaseapi.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <thread>

namespace xpcog::winui {

namespace {

namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;

constexpr const wchar_t* kInstanceKey = L"XPCog";

}  // namespace

bool claimInstance() {
    const lifecycle::AppInstance holder = lifecycle::AppInstance::FindOrRegisterForKey(kInstanceKey);
    if (holder.IsCurrent()) {
        return true;
    }

    // The player is already running. Let it take the foreground -- a window
    // can only bring itself forward with the permission of the one that has
    // it, which is this launch -- and hand it this activation.
    platform::permitForegroundHandover();

    // Redirection is asynchronous and must be waited for, or it is lost when
    // this process exits; but blocking an STA thread on it is what the
    // documentation warns against. So it runs on a thread of its own, and this
    // one waits in CoWaitForMultipleObjects, which keeps COM pumping.
    const HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread redirect([holder, done] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            holder.RedirectActivationToAsync(lifecycle::AppInstance::GetCurrent().GetActivatedEventArgs())
                .get();
        } catch (const winrt::hresult_error&) {
            // The holder exited between the lookup and the redirect. Nothing
            // to do but go: the next launch will find no holder and run.
        }
        ::SetEvent(done);
    });
    DWORD index = 0;
    ::CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, &done, &index);
    redirect.join();
    ::CloseHandle(done);
    return false;
}

std::vector<Url> urlsFromCommandLine(const winrt::hstring& commandLine) {
    std::vector<Url> urls;
    if (commandLine.empty()) {
        return urls;
    }
    int     count = 0;
    LPWSTR* words = ::CommandLineToArgvW(commandLine.c_str(), &count);
    if (words == nullptr) {
        return urls;
    }

    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path executable(self);

    for (int i = 0; i < count; ++i) {
        const std::filesystem::path path(words[i]);
        std::error_code error;
        // The executable itself: an unpackaged launch's arguments are its
        // whole command line, program name first.
        if (std::filesystem::equivalent(path, executable, error)) {
            continue;
        }
        if (std::filesystem::exists(path, error)) {
            urls.push_back(Url::fromLocalPath(std::filesystem::absolute(path, error)));
            continue;
        }
        // Not a file. A URL, if it has a real scheme: a drive letter would
        // parse as a one-letter one, and a path that does not exist is not
        // something to hand the network.
        if (const std::optional<Url> url = Url::parse(winrt::to_string(words[i]));
            url && url->scheme().size() > 1) {
            urls.push_back(*url);
        }
    }
    ::LocalFree(words);
    return urls;
}

}  // namespace xpcog::winui
