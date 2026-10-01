#pragma once

// The WinRT half of WinUIIsland.hpp: the projection, and the island's host
// state, for the sources that build what an island shows.

#include "WinUIIsland.hpp"

#include <wx/string.h>

// <windows.h> has arrived through wx by now, and winbase.h defines
// GetCurrentTime as a macro, which breaks the Storyboard method of that name in
// the XAML projection. The documented workaround is exactly this.
#undef GetCurrentTime

#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace xpcog::app {

namespace mux = winrt::Microsoft::UI::Xaml;

struct WinUIIsland::Host {
    mux::Hosting::DesktopWindowXamlSource source{nullptr};
};

/// "what (0x80004005): message", for winUIFailure().
wxString describeWinRTError(const char* what, const winrt::hresult_error& error);

}  // namespace xpcog::app
