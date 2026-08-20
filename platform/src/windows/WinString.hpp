// UTF-8 to UTF-16 and back, for the Windows half of this layer.
//
// Every string that crosses into this layer is UTF-8 -- that is what core uses
// and what the public headers now say -- and every wide Win32 entry point wants
// UTF-16. Qt used to absorb that; QString is UTF-16 already, so `toHString` was a
// copy and `toStdString` was the only conversion in sight.
//
// Doing it by hand is four lines each, and the reason to have them in one place
// is that the *wrong* way is so easy to reach for: the narrow Win32 entry points
// (RegSetValueExA and friends) take the active code page, not UTF-8, so a path or
// a tag with a non-ASCII character in it lands mangled and nothing reports an
// error. Nothing in this directory should call an -A function.

#pragma once

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace xpcog::platform {

[[nodiscard]] inline std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        needed);
    return wide;
}

[[nodiscard]] inline std::string toUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                        needed, nullptr, nullptr);
    return utf8;
}

}  // namespace xpcog::platform
