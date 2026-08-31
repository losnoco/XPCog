#pragma once

// The pitch and tempo sliders, as a dockable pane.
//
// Cog reaches these two from a toolbar button that opens an NSPopover
// (Window/SpeedButton.m). A popup was tried here first and looked wrong:
// wxWidgets' wxPopupTransientWindow is a bare rectangle with no arrow, no
// animation and no material behind it, which reads as a menu that lost its
// menu rather than as a panel. A dock pane is what the rest of this window
// already does with a group of controls -- the equaliser is the same shape of
// thing -- and it can be left open beside the playlist, which a transient
// popup cannot.
//
// The same two settings are still on the Pitch & Tempo preferences pane, which
// is where the engine and its dozen Rubber Band knobs live. This pane is the
// two controls worth reaching for mid-track, and a button through to the rest.

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/panel.h>

#include <string>

class wxButton;
class wxCheckBox;
class wxSlider;
class wxStaticText;

namespace xpcog::app {

class SpeedPanel : public wxPanel {
public:
    SpeedPanel(wxWindow* parent, Settings& settings);

    /// Rereads `pitch`, `tempo`, `speedLock` and the engine. For when the
    /// preferences pane moved them.
    void refresh();

    /// A setting changed; MainFrame reloads the DSP chain when it hears one.
    Signal<std::string> settingChanged;

    /// The button through to Preferences, opened on the Pitch & Tempo pane.
    Signal<> settingsRequested;

private:
    void write(const char* key, double ratio);
    void showValue(wxStaticText* label, double ratio);

    Settings&     settings_;
    wxSlider*     pitch_      = nullptr;
    wxSlider*     tempo_      = nullptr;
    wxStaticText* pitchLabel_ = nullptr;
    wxStaticText* tempoLabel_ = nullptr;
    wxStaticText* pitchValue_ = nullptr;
    wxStaticText* tempoValue_ = nullptr;
    wxCheckBox*   lock_       = nullptr;
    wxButton*     reset_      = nullptr;
    wxStaticText* note_       = nullptr;
};

}  // namespace xpcog::app
