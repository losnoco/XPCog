#include "Tray.hpp"

#include "Translations.hpp"

#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwchar>

namespace xpcog::winui {
namespace {

constexpr UINT     kCallbackMessage = WM_APP + 1;
constexpr UINT     kIconId          = 1;
constexpr wchar_t  kWindowClass[]   = L"XPCog.WinUI.Tray";
/// The icon in XPCog.rc.in.
constexpr wchar_t  kIconResource[]  = L"AAAA_XPCOG";
/// A balloon's own picture, in pixels: the size a large notification icon is
/// drawn at, so the shell does not scale a cover the size of a poster.
constexpr int      kBalloonImageSide = 48;

std::wstring widen(const std::string& utf8) {
    return std::wstring(winrt::to_hstring(utf8));
}

/// `text` into a fixed WCHAR field, cut to fit rather than overrunning it.
template <std::size_t N>
void copyInto(wchar_t (&field)[N], const std::wstring& text) {
    const std::size_t length = std::min(text.size(), N - 1);
    std::wmemcpy(field, text.data(), length);
    field[length] = L'\0';
}

/// An icon made of encoded image bytes -- a cover -- scaled to `side`, or null
/// when they are not an image WIC can read.
HICON iconFromImage(std::span<const std::byte> bytes, int side) {
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put())))) {
        return nullptr;
    }
    winrt::com_ptr<IStream> stream;
    stream.attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                      static_cast<UINT>(bytes.size())));
    winrt::com_ptr<IWICBitmapDecoder>     decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICBitmapScaler>      scaler;
    winrt::com_ptr<IWICFormatConverter>   converter;
    if (!stream ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) ||
        FAILED(factory->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(frame.get(), side, side, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = side;
    info.bmiHeader.biHeight      = -side;  // top-down, as WIC writes it
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void*   bits   = nullptr;
    HBITMAP colour = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (colour == nullptr) {
        return nullptr;
    }
    const UINT stride = static_cast<UINT>(side) * 4;
    HICON      icon   = nullptr;
    if (SUCCEEDED(converter->CopyPixels(nullptr, stride, stride * side, static_cast<BYTE*>(bits)))) {
        // The mask is ignored for a 32-bit colour bitmap with alpha, but
        // CreateIconIndirect will not go without one.
        HBITMAP  mask = ::CreateBitmap(side, side, 1, 1, nullptr);
        ICONINFO parts{TRUE, 0, 0, mask, colour};
        icon = ::CreateIconIndirect(&parts);
        ::DeleteObject(mask);
    }
    ::DeleteObject(colour);
    return icon;
}

}  // namespace

Tray::Tray() {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    static const ATOM registered = [instance] {
        WNDCLASSEXW cls{};
        cls.cbSize        = sizeof(cls);
        cls.lpfnWndProc   = &Tray::windowProc;
        cls.hInstance     = instance;
        cls.lpszClassName = kWindowClass;
        return ::RegisterClassExW(&cls);
    }();
    if (registered == 0) {
        return;
    }
    // Never shown: it exists to receive the icon's messages and to own its menu.
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"XPCog", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return;
    }
    taskbarCreated_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    // At the notification area's own size, at the system's scale, rather than
    // a 32-pixel icon squeezed down. LoadImage rather than LoadIconMetric,
    // which comctl32 exports only by ordinal and only in version 6 -- and this
    // executable's manifest does not ask for version 6, so the loader binds
    // 5.82 and refuses to start the program at all.
    const UINT dpi  = ::GetDpiForSystem();
    const int  side = ::GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    icon_ = static_cast<HICON>(::LoadImageW(instance, kIconResource, IMAGE_ICON, side, side, 0));
    shown_ = add();
}

Tray::~Tray() {
    remove();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
    }
    if (icon_ != nullptr) {
        ::DestroyIcon(icon_);
    }
    if (balloon_ != nullptr) {
        ::DestroyIcon(balloon_);
    }
}

bool Tray::add() {
    NOTIFYICONDATAW data{};
    data.cbSize           = sizeof(data);
    data.hWnd             = hwnd_;
    data.uID              = kIconId;
    data.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon            = icon_;
    copyInto(data.szTip, L"XPCog");
    if (!::Shell_NotifyIconW(NIM_ADD, &data)) {
        return false;
    }
    // Version 4: a click is NIN_SELECT, the menu key and a right-click are
    // WM_CONTEXTMENU, and the tooltip is the shell's own.
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);
    update();
    return true;
}

void Tray::remove() {
    if (!shown_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd   = hwnd_;
    data.uID    = kIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    shown_ = false;
}

void Tray::setState(const app::TrayState& state) {
    state_ = state;
    update();
}

void Tray::update() {
    if (hwnd_ == nullptr) {
        return;
    }
    // "XPCog" over the track, as the wx tray's title and body. The menu needs
    // no pushing: it is built from the state when it is opened.
    std::wstring tip = L"XPCog";
    if (const std::string body = app::trayTooltipBody(state_); !body.empty()) {
        tip += L"\n" + widen(body);
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd   = hwnd_;
    data.uID    = kIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    copyInto(data.szTip, tip);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::notify(const std::string& title, const std::string& body,
                  std::span<const std::byte> image) {
    if (!shown_) {
        return;
    }
    if (balloon_ != nullptr) {
        ::DestroyIcon(balloon_);
        balloon_ = nullptr;
    }
    if (!image.empty()) {
        balloon_ = iconFromImage(image, kBalloonImageSide);
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd   = hwnd_;
    data.uID    = kIconId;
    data.uFlags = NIF_INFO;
    copyInto(data.szInfoTitle, widen(title));
    copyInto(data.szInfo, widen(body));
    // Silent: one of these comes with every track, and the music is the sound.
    data.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;
    if (balloon_ != nullptr) {
        data.dwInfoFlags |= NIIF_LARGE_ICON;
        data.hBalloonIcon = balloon_;
    }
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::showMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    constexpr bool withWindowItems = true;
    for (const platform::TrayMenuItem& item : app::trayMenuModel(state_, withWindowItems)) {
        if (item.separator) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const UINT flags = MF_STRING | (item.enabled ? 0 : MF_GRAYED);
        ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), widen(item.label).c_str());
    }

    POINT at{};
    ::GetCursorPos(&at);
    // The documented dance for a notification-area menu: foreground first, or
    // it does not close when clicked away from; a message after, or it does
    // not open a second time (TrackPopupMenu's remarks).
    ::SetForegroundWindow(hwnd_);
    const UINT align = ::GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const int  chosen = static_cast<int>(::TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, at.x, at.y,
        hwnd_, nullptr));
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    if (chosen != 0 && commandChosen) {
        commandChosen(chosen);
    }
}

LRESULT CALLBACK Tray::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<Tray*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->handle(hwnd, message, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Tray::handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCallbackMessage) {
        switch (LOWORD(lParam)) {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                if (activated) {
                    activated();
                }
                return 0;
            case WM_CONTEXTMENU:
                showMenu();
                return 0;
            default:
                return 0;
        }
    }
    // Explorer restarted, and took the icon with it.
    if (message == taskbarCreated_ && taskbarCreated_ != 0) {
        shown_ = add();
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace xpcog::winui
