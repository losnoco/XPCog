#pragma once

// The pitch and tempo sliders, on a popup hung off the transport strip.
//
// Cog puts these two on an NSPopover opened from a toolbar button
// (Window/SpeedButton.m), which is where a control you reach for *while
// listening* belongs -- the preferences pane is two clicks and a window away
// from the thing being adjusted. wxWidgets' nearest equivalent is
// wxPopupTransientWindow: a borderless top-level that dismisses itself on a
// click outside or any other loss of focus, which is NSPopoverBehaviorTransient
// by another name.
//
// What it is not is an NSPopover. There is no arrow pointing back at the
// button, no present/dismiss animation and no vibrancy behind it -- this is a
// plain bordered rectangle, positioned by hand under the button.
//
// The same sliders remain on the Pitch & Tempo pane, and both write the same
// two settings through the same curve in SpeedCurve.hpp.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/popupwin.h>

#include <string>

class wxCheckBox;
class wxSlider;
class wxStaticText;

namespace xpcog::app {

class SpeedPopup : public wxPopupTransientWindow {
public:
    SpeedPopup(wxWindow* parent, Settings& settings);

    /// Shows the popup under `anchor`, left edges aligned, kept on screen.
    void popupUnder(wxWindow* anchor);

    /// Rereads `pitch`, `tempo` and `speedLock` from the settings. For when the
    /// preferences pane moved them while this was closed.
    void refresh();

    /// The same signal the preferences dialog carries, and for the same reason:
    /// the engine rereads its DSP chain when MainFrame hears one of these keys.
    Signal<std::string> settingChanged;

private:
    void write(const char* key, double ratio);
    void showValue(wxStaticText* label, double ratio);

    Settings&     settings_;
    wxSlider*     pitch_      = nullptr;
    wxSlider*     tempo_      = nullptr;
    wxStaticText* pitchValue_ = nullptr;
    wxStaticText* tempoValue_ = nullptr;
    wxCheckBox*   lock_       = nullptr;
    wxWindow*     disabled_   = nullptr;
};

}  // namespace xpcog::app
