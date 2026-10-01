#include "Win2D.hpp"

#include "WinRT.hpp"

#include <roapi.h>
#include <winstring.h>

#include <string_view>

namespace xpcog::winui {

namespace {

using GetFactory = HRESULT(__stdcall*)(HSTRING, void**);

constexpr std::wstring_view kWin2DNamespace = L"Microsoft.Graphics.Canvas.";

int32_t __stdcall activate(void* classId, winrt::guid const& iid, void** factory) noexcept {
    *factory = nullptr;

    UINT32         length = 0;
    const wchar_t* name   = ::WindowsGetStringRawBuffer(static_cast<HSTRING>(classId), &length);
    if (std::wstring_view(name, length).starts_with(kWin2DNamespace)) {
        // Loaded once and never freed: the classes it makes live as long as
        // the process does.
        static const GetFactory get = [] {
            const HMODULE module = ::LoadLibraryExW(L"Microsoft.Graphics.Canvas.dll", nullptr,
                                                    LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            return module != nullptr ? reinterpret_cast<GetFactory>(
                                           ::GetProcAddress(module, "DllGetActivationFactory"))
                                     : nullptr;
        }();
        if (get == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }
        // DllGetActivationFactory answers with IActivationFactory; the caller
        // asked for `iid`, which is usually a class's statics or its factory.
        winrt::com_ptr<::IUnknown> activation;
        const HRESULT hr = get(static_cast<HSTRING>(classId), activation.put_void());
        if (FAILED(hr)) {
            return hr;
        }
        return activation->QueryInterface(reinterpret_cast<const GUID&>(iid), factory);
    }

    // Everything else as C++/WinRT would have done it with no hook installed.
    return ::RoGetActivationFactory(static_cast<HSTRING>(classId), reinterpret_cast<const GUID&>(iid),
                                    factory);
}

}  // namespace

void installWin2DActivation() {
    winrt_activation_handler = &activate;
}

}  // namespace xpcog::winui
