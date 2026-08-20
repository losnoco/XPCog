// The 31-band equaliser, as a panel rather than a preferences pane.
//
// It began life inside the preferences dialog, which was the wrong home for it.
// A preferences dialog is something you close: everything else in there is set
// once and forgotten, whereas an equaliser is played *with* -- adjusted while
// listening, against audio you can hear change. Behind a dialog that means
// opening preferences, finding the pane, dragging a slider, and having the thing
// you are judging half-hidden behind a window.
//
// Cog agrees, and always did: its equaliser is an EqualizerWindowController with
// a window of its own, not one of its ten preference panes. This is a panel the
// frame shows and hides, which is the same idea with less window management.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/scrolwin.h>

#include <string>
#include <vector>

class wxBoxSizer;
class wxSlider;
class wxStaticText;

namespace xpcog::app {

/// Scrolled horizontally, because 32 columns have a natural width the pane may
/// not have. Cog's equaliser lives in a window of its own and can simply be
/// made wide enough; a dock cannot, so the choice is between clipping the top
/// bands off -- which is what happened -- and scrolling to them.
class EqualizerPanel : public wxScrolled<wxPanel> {
public:
    EqualizerPanel(wxWindow* parent, Settings& settings);

    /// Every band and the preamp back to 0 dB, which makes the equaliser
    /// bit-transparent again -- a flat chain is skipped rather than run.
    void flatten();

    /// A band moved. Carries the setting key, so the frame can tell an equaliser
    /// change from any other and ask the engine to re-read the chain mid-track.
    Signal<std::string> settingChanged;

private:
    /// One labelled column. Reads and writes by settings key, so nothing here
    /// needs to know 31 accessor names and the band-to-key pairing stays where
    /// the DSP defines it.
    wxSlider* addBand(wxBoxSizer* columns, const std::string& caption,
                      const std::string& key);

    Settings&                  settings_;
    /// Parallel: `sliders_[i]`'s readout is `readouts_[i]`, and `keys_[i]` is the
    /// setting behind both. Three vectors rather than a struct because the
    /// slider list is also what flatten() walks, and the pairing is only needed
    /// there.
    std::vector<wxSlider*>     sliders_;
    std::vector<wxStaticText*> readouts_;
    std::vector<std::string>   keys_;
};

}  // namespace xpcog::app
