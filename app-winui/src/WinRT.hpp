#pragma once

// The projection, in one place, and the two conversions every file here needs.
//
// <windows.h> before anything else, then GetCurrentTime undefined: winbase.h
// defines it as a macro, which breaks the Storyboard method of that name in the
// XAML projection. The documented workaround is exactly this.

#include <windows.h>
#undef GetCurrentTime

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <string>
#include <string_view>

namespace xpcog::winui {

namespace mux = winrt::Microsoft::UI::Xaml;

/// UTF-8, which is what core and uicore speak, to the UTF-16 WinRT wants.
[[nodiscard]] inline winrt::hstring toH(std::string_view utf8) {
    return winrt::to_hstring(utf8);
}

[[nodiscard]] inline std::string toUtf8(const winrt::hstring& text) {
    return winrt::to_string(text);
}

/// XAML text, parsed. For what code cannot say: chiefly {ThemeResource}
/// references, which follow a light/dark switch where a brush looked up from
/// code stays the colour of the moment it was looked up.
template <typename T>
[[nodiscard]] T loadXaml(const std::wstring& xaml) {
    return mux::Markup::XamlReader::Load(xaml).as<T>();
}

/// A content-layer card: LayerFillColorDefault over the Mica, with the card
/// stroke and corner radius the Windows 11 layering guidance gives it. What
/// every region holding content sits on, so they read as one surface.
[[nodiscard]] inline mux::Controls::Border card() {
    return loadXaml<mux::Controls::Border>(
        L"<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
        L" Background='{ThemeResource LayerFillColorDefaultBrush}'"
        L" BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}'"
        L" BorderThickness='1' CornerRadius='8'/>");
}

/// A secondary-text TextBlock: captions, readouts, notes.
[[nodiscard]] inline mux::Controls::TextBlock secondaryText(const wchar_t* extra = L"") {
    return loadXaml<mux::Controls::TextBlock>(
        std::wstring(L"<TextBlock xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'"
                     L" Foreground='{ThemeResource TextFillColorSecondaryBrush}' ") +
        extra + L"/>");
}

/// A brush from the theme resources by key. Looked up when called, so it is the
/// current theme's -- which is the reason to ask rather than to build a colour.
[[nodiscard]] inline mux::Media::Brush themeBrush(const wchar_t* key) {
    return mux::Application::Current()
        .Resources()
        .Lookup(winrt::box_value(key))
        .as<mux::Media::Brush>();
}

}  // namespace xpcog::winui
