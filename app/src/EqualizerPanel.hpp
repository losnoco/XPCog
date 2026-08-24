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
//
// The preset selector sits above the sliders and is the same control Cog's
// window has, with the same two-way relationship: choosing a preset writes the
// curve, and moving any slider afterwards drops the selection to "Custom"
// because the curve is no longer the preset's. Nothing here holds a curve of its
// own -- the settings are the record, the sliders are a view of them, and a
// preset is one more way of writing them. That is what lets genre tracking
// change the curve from outside this panel and have refresh() be the whole of
// the panel's response.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/scrolwin.h>

#include <string>
#include <vector>

class wxBoxSizer;
class wxCheckBox;
class wxChoice;
class wxSlider;
class wxStaticText;

namespace xpcog {
class EqualizerPresetLibrary;
}

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
    ///
    /// Routed through the preset named "Flat" when the library has one, which is
    /// what Cog's Flat button does and is not merely a longer way of writing
    /// zeroes: it leaves the selector reading "Flat" rather than "Custom", so
    /// the state the user is looking at says which preset produced the curve.
    void flatten();

    /// Re-reads the sliders, the selector and the genre checkbox from the
    /// settings.
    ///
    /// For when something else wrote the curve. Today that is genre tracking and
    /// nothing else, and it is why the panel keeps no copy of the values: a
    /// second writer means any cached curve is one track change away from being
    /// a lie.
    void refresh();

    /// A setting under this panel changed. Carries the key, so the frame can
    /// tell an equaliser change from any other and ask the engine to re-read the
    /// chain mid-track.
    Signal<std::string> settingChanged;

private:
    /// One labelled column. Reads and writes by settings key, so nothing here
    /// needs to know 31 accessor names and the band-to-key pairing stays where
    /// the DSP defines it.
    wxSlider* addBand(wxBoxSizer* columns, const std::string& caption,
                      const std::string& key);

    /// The preset row: the selector and the genre checkbox. Built empty when no
    /// library was found, because a selector offering only "Custom" is a control
    /// that can do nothing.
    void buildPresetRow(wxBoxSizer* layout);

    /// Applies the preset at `index`, writing the curve into the settings.
    /// Selecting the "Custom" row records the choice and leaves the curve alone,
    /// which is Cog's behaviour -- Custom is not a preset, it is the absence of
    /// one.
    void selectPreset(int index);

    /// The curve stopped being a preset's, because a slider moved.
    void markCustom();

    /// Sliders and readouts from the settings, without publishing. The selector
    /// and checkbox too.
    void syncFromSettings();

    /// Tells the frame that every key under this panel changed. Cheap enough to
    /// do wholesale: the engine's response is to set one flag, so 32 of them
    /// coalesce into the single re-read the next DSP pass performs.
    void publishCurve();

    /// The index of the "Custom" row, which is one past the last preset.
    [[nodiscard]] int customIndex() const;

    Settings&                     settings_;
    const EqualizerPresetLibrary& presets_;

    wxChoice*   presetChoice_ = nullptr;
    wxCheckBox* trackGenre_   = nullptr;

    /// Parallel: `sliders_[i]`'s readout is `readouts_[i]`, and `keys_[i]` is the
    /// setting behind both. Three vectors rather than a struct because the
    /// slider list is also what flatten() and syncFromSettings() walk, and the
    /// pairing is only needed there. Index 0 is the preamp; the 31 bands follow.
    std::vector<wxSlider*>     sliders_;
    std::vector<wxStaticText*> readouts_;
    std::vector<std::string>   keys_;
};

}  // namespace xpcog::app
