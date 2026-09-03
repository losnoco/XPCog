#include <xpcog/platform/AccentColour.hpp>

#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/base.h>

namespace xpcog::platform {

std::optional<AccentRgb> accentColour() {
    // UISettings rather than DwmGetColorizationColor. The two are not the same
    // number: DWM's is the title-bar tint, which the user can have the system
    // derive from the wallpaper and which carries an alpha that is not a
    // transparency. UIColorType::Accent is the colour the Settings app calls the
    // accent colour and the one every modern Windows control is tinted with.
    //
    // Wrapped, because this is a WinRT activation and the failure modes are
    // environmental rather than logical: no apartment initialised on this thread,
    // or a session with no user profile behind it. Both mean "no accent to be
    // had", which is precisely what nullopt says.
    try {
        const winrt::Windows::UI::ViewManagement::UISettings settings;
        const winrt::Windows::UI::Color colour =
            settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        return AccentRgb{colour.R, colour.G, colour.B};
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

}  // namespace xpcog::platform
