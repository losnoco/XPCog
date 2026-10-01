// The settings whose values are a closed set.
//
// Every one of these is a list of Cog's own stored values with a label beside
// it, and neither half belongs to a toolkit: the **values** are what goes in the
// settings file and are never translated -- writing a Spanish word there would
// produce a file Cog cannot read -- and the labels are msgids like any other.
//
// Here rather than in a dialog because there are two dialogs now. A list that
// each frontend kept its own copy of would drift the first time a value was
// added, and the failure would be a picker that silently cannot select what the
// other frontend wrote.

#pragma once

#include "Translations.hpp"

#include <array>
#include <span>
#include <string_view>

namespace xpcog::app {

/// A setting whose values are a closed set, so it deserves a named list rather
/// than a text box. The **values** are Cog's stored values, unchanged; only the
/// labels beside them are language. Translating a value would write a Spanish
/// word into a settings file Cog is expected to be able to read.
struct Choice {
    const char* value;
    const char* label;  ///< marked with wxTRANSLATE; looked up by choice()
};

inline constexpr std::array kVolumeScalingChoices = {
    Choice{"none", XPCOG_TRANSLATE("None")},
    Choice{"volumeScale", XPCOG_TRANSLATE("Volume tag")},
    Choice{"soundcheck", XPCOG_TRANSLATE("iTunes Sound Check")},
    Choice{"trackGain", XPCOG_TRANSLATE("Track gain")},
    Choice{"trackGainWithPeak", XPCOG_TRANSLATE("Track gain, peak-limited")},
    Choice{"albumGain", XPCOG_TRANSLATE("Album gain")},
    Choice{"albumGainWithPeak", XPCOG_TRANSLATE("Album gain, peak-limited")},
};

inline constexpr std::array kResamplingChoices = {
    Choice{"quick", XPCOG_TRANSLATE("Quick")}, Choice{"low", XPCOG_TRANSLATE("Low")},   Choice{"medium", XPCOG_TRANSLATE("Medium")},
    Choice{"high", XPCOG_TRANSLATE("High")},   Choice{"best", XPCOG_TRANSLATE("Best")},
};

// The stretch engines and the Rubber Band option vocabularies, values and
// defaults from Cog's Rubber Band pane (Preferences/Panes/RubberbandPaneView
// .swift). `varispeed` is ours: Cog has no resampling speed control.
inline constexpr std::array kStretchEngineChoices = {
    Choice{"disabled", XPCOG_TRANSLATE("Disabled")},
    Choice{"varispeed", XPCOG_TRANSLATE("Varispeed \xE2\x80\x94 resample, pitch follows tempo")},
    Choice{"signalsmith", XPCOG_TRANSLATE("Signalsmith Stretch")},
    Choice{"faster", XPCOG_TRANSLATE("Rubber Band \xE2\x80\x94 Faster")},
    Choice{"finer", XPCOG_TRANSLATE("Rubber Band \xE2\x80\x94 Finer")},
};
inline constexpr std::array kRubberTransientsChoices = {
    Choice{"crisp", XPCOG_TRANSLATE("Crisp")}, Choice{"mixed", XPCOG_TRANSLATE("Mixed")}, Choice{"smooth", XPCOG_TRANSLATE("Smooth")},
};
inline constexpr std::array kRubberDetectorChoices = {
    Choice{"compound", XPCOG_TRANSLATE("Compound")},
    Choice{"percussive", XPCOG_TRANSLATE("Percussive")},
    Choice{"soft", XPCOG_TRANSLATE("Soft")},
};
inline constexpr std::array kRubberPhaseChoices = {
    Choice{"laminar", XPCOG_TRANSLATE("Laminar")},
    Choice{"independent", XPCOG_TRANSLATE("Independent")},
};
inline constexpr std::array kRubberWindowChoices = {
    Choice{"standard", XPCOG_TRANSLATE("Standard")}, Choice{"short", XPCOG_TRANSLATE("Short")}, Choice{"long", XPCOG_TRANSLATE("Long")},
};
inline constexpr std::array kRubberSmoothingChoices = {
    Choice{"off", XPCOG_TRANSLATE("Off")},
    Choice{"on", XPCOG_TRANSLATE("On")},
};
inline constexpr std::array kRubberFormantChoices = {
    Choice{"shifted", XPCOG_TRANSLATE("Shifted")},
    Choice{"preserved", XPCOG_TRANSLATE("Preserved")},
};
inline constexpr std::array kRubberPitchChoices = {
    Choice{"highspeed", XPCOG_TRANSLATE("High speed")},
    Choice{"highquality", XPCOG_TRANSLATE("High quality")},
    Choice{"highconsistency", XPCOG_TRANSLATE("High consistency")},
};
inline constexpr std::array kRubberChannelsChoices = {
    Choice{"apart", XPCOG_TRANSLATE("Apart")},
    Choice{"together", XPCOG_TRANSLATE("Together")},
};

/// The synthesisers `midiPlugin` can name, in Cog's own spelling.
///
/// Nuked OPL3 twice over -- id's DMX driver, once per instrument bank, and
/// Nuke.YKT's General MIDI one -- and then an emulated Roland. The OPL labels are
/// the drivers' own bank names (vendor/nuked-opl3), spelled out here rather than
/// read back from them so the dialog does not have to construct a synthesiser to
/// draw a list. See docs/MIDI.md.
inline constexpr std::array kMidiSynthChoices = {
    Choice{"DOOM0", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMX default")},
    Choice{"DOOM1", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMX Doom")},
    Choice{"DOOM2", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMX Doom II")},
    Choice{"DOOM3", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMX Raptor")},
    Choice{"DOOM4", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMX Strife")},
    Choice{"DOOM5", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 DMXOPL")},
    Choice{"OPL3W0", XPCOG_TRANSLATE("OPL3 \xE2\x80\x94 General MIDI")},
    Choice{"Spessa", XPCOG_TRANSLATE("SoundFont \xE2\x80\x94 SpessaSynth")},
    Choice{"NukeSc55", XPCOG_TRANSLATE("Roland SC-55")},
};

/// The choices for `key`, or an empty span when it is not one of these.
///
/// By key rather than by name, so a pane can ask "is this setting a picker?"
/// without a table of its own repeating which ones are.
[[nodiscard]] std::span<const Choice> choicesFor(std::string_view key);

/// The label beside `value` in `choices`, translated. The value itself when it
/// is not in the list -- a settings file carried from a build that had more.
[[nodiscard]] std::string choiceLabel(std::span<const Choice> choices,
                                      std::string_view        value);

/// Whether `key` has a hand-written row on one of the preference panes, so the
/// generated Advanced pane skips it and each setting is edited in exactly one
/// place. Both frontends' panes cover the same keys, which is what lets this
/// be one list.
[[nodiscard]] bool hasCuratedRow(std::string_view key);

/// Not a setting at all, but internal state that happens to live in the same
/// store: shown on Advanced, because its whole point is that nothing is
/// hidden, but not editable.
[[nodiscard]] bool isInternalKey(std::string_view key);

}  // namespace xpcog::app
