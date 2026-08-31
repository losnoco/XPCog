#include "EqualizerPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/translation.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace xpcog::app {
namespace {

/// Cog's slider range, in dB, for the bands and the preamp alike.
constexpr int kEqRangeDb = 20;
/// Sliders are integers, so dB is carried in tenths.
constexpr int kEqScale = 10;

/// The row at the end of the selector that stands for "this curve is not a
/// preset". Cog spells it this way and puts it last, after every preset.
/// The row that stands for "the curve is not one of the presets".
///
/// A wxTRANSLATE rather than a `_()`: it is a file-scope constant, so it has to
/// be a literal, and the lookup happens where it is appended to the list. The
/// preset *names* beside it are not translated -- they are the shipped library's
/// own, matched by name against a track's genre tag, and a translated "Rock"
/// would stop matching the tag that chose it.
constexpr const char* kCustomLabel = wxTRANSLATE("Custom");

/// "20", "31.5", "1k", "20k" -- short enough to sit under a narrow slider.
[[nodiscard]] std::string frequencyLabel(double hertz) {
    char buffer[16] = {};
    if (hertz < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%g", hertz);
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%gk", hertz / 1000.0);
    return buffer;
}

[[nodiscard]] std::string decibelLabel(int scaled) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(scaled) / kEqScale);
    return buffer;
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

}  // namespace

EqualizerPanel::EqualizerPanel(wxWindow* parent, Settings& settings)
    : wxScrolled<wxPanel>(parent, wxID_ANY),
      settings_(settings),
      presets_(shippedEqualizerPresets()) {
    // Horizontally only: the columns are as tall as they are and scrolling them
    // vertically would hide the readouts or the labels rather than help.
    SetScrollRate(FromDIP(8), 0);
    auto* layout = new wxBoxSizer(wxVERTICAL);

    buildPresetRow(layout);

    // One column per band, plus the preamp on its own at the left, matching the
    // shape of Cog's Equalizer window. Sliders are deliberately narrow: 31 bands
    // is a lot of screen, and the alternative -- a scrolling area the user has to
    // pan to reach 20 kHz -- makes a curve impossible to see as a curve.
    auto* columns = new wxBoxSizer(wxHORIZONTAL);

    // "Pre" is a caption under a slider and has room for about four
    // characters, which is why it is abbreviated in English too. The
    // translator's note in the .po says so; a language that cannot fit it
    // shortens rather than wraps.
    sliders_.push_back(addBand(columns, toUtf8(_("Pre")), "eqPreamp"));
    columns->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxLI_VERTICAL),
                 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));

    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        sliders_.push_back(addBand(columns, frequencyLabel(frequencies[band]), keys[band]));
    }

    layout->Add(columns, 1, wxEXPAND | wxALL, FromDIP(4));

    // The button's label, not the preset's name. The preset is called "Flat"
    // in the shipped library and is matched by that name in flatten() below, so
    // the two are deliberately different strings even though they read the same
    // in English -- translating the label must not stop the lookup working.
    auto* flat = new wxButton(this, wxID_ANY, _("Flat"));
    flat->SetToolTip(_("Selects the Flat preset: every band and the preamp back to "
                       "0 dB, which makes the equaliser bit-transparent again."));
    flat->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flatten(); });

    auto* note = new wxStaticText(
        this, wxID_ANY,
        trUtf8("31 bands, \xC2\xB1""20 dB. Changes apply to the track already playing. "
          "A boost can clip; the preamp is the headroom for it."));
    note->Wrap(FromDIP(420));
    note->Enable(false);

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(flat, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    footer->Add(note, 1, wxALIGN_CENTER_VERTICAL);
    layout->Add(footer, 0, wxEXPAND | wxALL, FromDIP(4));

    SetSizer(layout);

    // After the controls exist, because it writes into all of them.
    syncFromSettings();

    // The virtual size, which is what the scrollbar is calculated from. Without
    // it the panel reports the pane's width as its content width and never
    // scrolls -- it just clips, which is the bug this replaced.
    FitInside();
}

void EqualizerPanel::buildPresetRow(wxBoxSizer* layout) {
    // No library, no row. That is a headless or broken install rather than
    // anything a user did, and the sliders work perfectly well without it -- a
    // selector whose only entry is "Custom" would just be a control that cannot
    // be used for anything.
    if (presets_.empty()) {
        return;
    }

    enabled_ = new wxCheckBox(this, wxID_ANY, _("Enable"));
    enabled_->SetToolTip(
        _("Bypasses the equaliser without disturbing the curve, which is what "
          "comparing one against the original needs. A flat equaliser is skipped "
          "either way, so this costs nothing until a band is moved."));
    enabled_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setGraphicEqEnable(event.IsChecked());
        settingChanged.publish("GraphicEQenable");
    });

    presetChoice_ = new wxChoice(this, wxID_ANY);
    for (const EqualizerPreset& preset : presets_.presets()) {
        presetChoice_->Append(toWx(preset.name));
    }
    presetChoice_->Append(trUtf8(kCustomLabel));
    presetChoice_->SetToolTip(
        _("Presets store ten points; the 31 bands are interpolated from them. "
          "Moving any slider afterwards leaves the curve alone and changes this "
          "to Custom."));
    presetChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& event) {
        selectPreset(event.GetSelection());
    });

    trackGenre_ = new wxCheckBox(this, wxID_ANY, _("Follow the track's genre"));
    trackGenre_->SetToolTip(
        _("Chooses the preset whose name matches each track's genre tag as it "
          "starts. A track with no genre, or one nothing matches, gets Flat -- so "
          "this rewrites the equaliser at every track boundary rather than only "
          "when it has something to say."));
    trackGenre_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setGraphicEqTrackGenre(event.IsChecked());
        // Published so the frame can apply the playing track's genre at once.
        // Waiting for the next track would make the checkbox look inert against
        // the audio it is meant to change.
        settingChanged.publish("GraphicEQtrackgenre");
    });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(enabled_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(new wxStaticText(this, wxID_ANY, _("Preset")), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    row->Add(presetChoice_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(trackGenre_, 0, wxALIGN_CENTER_VERTICAL);
    layout->Add(row, 0, wxEXPAND | wxALL, FromDIP(4));
}

wxSlider* EqualizerPanel::addBand(wxBoxSizer* columns, const std::string& caption,
                                  const std::string& key) {
    auto* column = new wxBoxSizer(wxVERTICAL);

    auto* readout = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                     FromDIP(wxSize(30, -1)),
                                     wxALIGN_CENTRE_HORIZONTAL);

    // wxSL_INVERSE is not decoration: a vertical wxSlider puts its minimum at the
    // *top* by default, so without it every band reads upside down -- a boost
    // drags the handle down. Qt's vertical slider is the other way round, which
    // is why this has no counterpart in the file it was ported from.
    //
    // The height is asked for and the width is not. A narrow slider is what makes
    // 32 columns fit, but the control has a minimum of its own -- a GtkScale is
    // 34 pixels wide before it will draw, an 18-pixel handle plus its margins --
    // and forcing 24 on it does not make it narrower, it makes it *absent*: GTK
    // measures the trough against the width it was given, finds nothing to work
    // with, warns "for_size smaller than min-size (0 < 4)" once per slider and
    // draws no scale at all. Thirty-two of those is an equaliser pane with the
    // readouts and the frequency labels in it and no sliders between them.
    auto* slider = new wxSlider(this, wxID_ANY, 0, -kEqRangeDb * kEqScale,
                                kEqRangeDb * kEqScale, wxDefaultPosition,
                                wxSize(wxDefaultCoord, FromDIP(140)),
                                wxSL_VERTICAL | wxSL_INVERSE);
    slider->SetToolTip(toWx(caption));

    slider->Bind(wxEVT_SLIDER, [this, key, readout](wxCommandEvent& event) {
        const int scaled = event.GetInt();
        readout->SetLabelText(toWx(decibelLabel(scaled)));
        settings_.setRawValue(key, std::to_string(static_cast<double>(scaled) / kEqScale));
        enableIfSilent();
        markCustom();
        settingChanged.publish(key);
    });

    auto* label = new wxStaticText(this, wxID_ANY, toWx(caption), wxDefaultPosition,
                                   FromDIP(wxSize(30, -1)), wxALIGN_CENTRE_HORIZONTAL);

    column->Add(readout, 0, wxALIGN_CENTER_HORIZONTAL);
    column->Add(slider, 1, wxALIGN_CENTER_HORIZONTAL);
    column->Add(label, 0, wxALIGN_CENTER_HORIZONTAL);
    columns->Add(column, 0, wxEXPAND);

    readouts_.push_back(readout);
    keys_.push_back(key);
    return slider;
}

wxSize EqualizerPanel::contentSize() const {
    wxSizer* layout = GetSizer();
    if (layout == nullptr) {
        return GetBestSize();
    }

    wxSize size = layout->GetMinSize();
    // Plus the horizontal scrollbar, which appears the moment the pane is
    // narrower than the curve -- which is most of the time, and it comes out of
    // the height the columns were measured for.
    size.y += wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y, this);
    return size;
}

int EqualizerPanel::customIndex() const { return static_cast<int>(presets_.size()); }

void EqualizerPanel::syncFromSettings() {
    for (std::size_t i = 0; i < sliders_.size(); ++i) {
        const int scaled = static_cast<int>(
            std::lround(toDouble(settings_.rawValue(keys_[i])) * kEqScale));
        sliders_[i]->SetValue(scaled);
        readouts_[i]->SetLabelText(toWx(decibelLabel(scaled)));
    }

    if (presetChoice_ != nullptr) {
        // Anything that is not an index into the library reads as Custom, which
        // covers both of the values that mean "no preset": the -1 default, and
        // the Custom row's own index.
        const int stored = settings_.GraphicEqPreset();
        presetChoice_->SetSelection(presets_.at(stored) != nullptr ? stored
                                                                  : customIndex());
    }
    if (trackGenre_ != nullptr) {
        trackGenre_->SetValue(settings_.GraphicEqTrackGenre());
    }
    if (enabled_ != nullptr) {
        enabled_->SetValue(settings_.GraphicEqEnable());
    }
}

void EqualizerPanel::publishCurve() {
    for (const std::string& key : keys_) {
        settingChanged.publish(key);
    }
}

void EqualizerPanel::selectPreset(int index) {
    settings_.setGraphicEqPreset(index);

    const EqualizerPreset* preset = presets_.at(index);
    if (preset == nullptr) {
        // The Custom row. Cog's changePreset() ignores it for the same reason
        // there is nothing to apply: Custom names no curve, it only records that
        // the curve came from somewhere other than a preset.
        return;
    }

    applyEqualizerPreset(settings_, *preset);
    // For the reason a moved slider does: picking "Bass Booster" and hearing
    // nothing is the same trap. Flat is the exception -- it is what somebody
    // reaches for to *stop* hearing the equaliser, so switching it on to deliver
    // a curve that does nothing would be perverse.
    if (preset->name != "Flat") {
        enableIfSilent();
    }
    syncFromSettings();
    publishCurve();
}

void EqualizerPanel::markCustom() {
    // With no library there is no Custom row, and writing its index would be
    // actively wrong rather than merely useless: customIndex() is 0 for an empty
    // library, and 0 is a real preset the moment a library turns up. The -1 the
    // setting already holds says "not a preset" correctly and keeps saying it.
    if (presetChoice_ == nullptr) {
        return;
    }
    const int custom = customIndex();
    if (settings_.GraphicEqPreset() == custom) {
        return;
    }
    settings_.setGraphicEqPreset(custom);
    presetChoice_->SetSelection(custom);
}

void EqualizerPanel::enableIfSilent() {
    if (settings_.GraphicEqEnable()) {
        return;
    }
    settings_.setGraphicEqEnable(true);
    if (enabled_ != nullptr) {
        enabled_->SetValue(true);
    }
    // Not published: the caller publishes the band key it just wrote, and the
    // frame's response to either is the same single reload.
}

void EqualizerPanel::refresh() { syncFromSettings(); }

void EqualizerPanel::flatten() {
    // Through the preset when there is one, which is what Cog's Flat button
    // does. It reaches the same 32 zeroes, and it leaves the selector saying
    // "Flat" rather than "Custom" -- the curve and what the interface says about
    // it stay the same statement.
    if (const int flat = presets_.indexOf("Flat"); flat >= 0) {
        selectPreset(flat);
        return;
    }

    // No library to route through. Every band explicitly, and that is the one
    // real difference from the Qt version: there, setValue() emitted
    // valueChanged, so writing the sliders wrote the settings and told the
    // engine as a side effect. wxSlider::SetValue raises **no** event --
    // programmatic changes never do in wx -- so relying on it would leave the
    // sliders flat and the audio unchanged.
    for (std::size_t i = 0; i < sliders_.size(); ++i) {
        sliders_[i]->SetValue(0);
        readouts_[i]->SetLabelText(toWx(decibelLabel(0)));
        settings_.setRawValue(keys_[i], "0");
        settingChanged.publish(keys_[i]);
    }
}

}  // namespace xpcog::app
