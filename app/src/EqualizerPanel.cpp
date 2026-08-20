#include "EqualizerPanel.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/Equalizer.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace xpcog::app {
namespace {

/// Cog's slider range, in dB, for the bands and the preamp alike.
constexpr int kEqRangeDb = 20;
/// Sliders are integers, so dB is carried in tenths.
constexpr int kEqScale = 10;

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
    : wxPanel(parent, wxID_ANY), settings_(settings) {
    auto* layout = new wxBoxSizer(wxVERTICAL);

    // One column per band, plus the preamp on its own at the left, matching the
    // shape of Cog's Equalizer window. Sliders are deliberately narrow: 31 bands
    // is a lot of screen, and the alternative -- a scrolling area the user has to
    // pan to reach 20 kHz -- makes a curve impossible to see as a curve.
    auto* columns = new wxBoxSizer(wxHORIZONTAL);

    sliders_.push_back(addBand(columns, "Pre", "eqPreamp"));
    columns->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxLI_VERTICAL),
                 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(3));

    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        sliders_.push_back(addBand(columns, frequencyLabel(frequencies[band]), keys[band]));
    }

    layout->Add(columns, 1, wxEXPAND | wxALL, FromDIP(4));

    auto* flat = new wxButton(this, wxID_ANY, "Flat");
    flat->SetToolTip("Returns every band and the preamp to 0 dB, which makes the "
                     "equaliser bit-transparent again.");
    flat->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { flatten(); });

    auto* note = new wxStaticText(
        this, wxID_ANY,
        "31 bands, \xC2\xB1""20 dB. Changes apply to the track already playing. "
        "A boost can clip; the preamp is the headroom for it.");
    note->Wrap(FromDIP(420));
    note->Enable(false);

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(flat, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    footer->Add(note, 1, wxALIGN_CENTER_VERTICAL);
    layout->Add(footer, 0, wxEXPAND | wxALL, FromDIP(4));

    SetSizer(layout);
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
    auto* slider = new wxSlider(this, wxID_ANY, 0, -kEqRangeDb * kEqScale,
                                kEqRangeDb * kEqScale, wxDefaultPosition,
                                FromDIP(wxSize(24, 140)), wxSL_VERTICAL | wxSL_INVERSE);
    slider->SetToolTip(toWx(caption));
    slider->SetValue(
        static_cast<int>(std::lround(toDouble(settings_.rawValue(key)) * kEqScale)));
    readout->SetLabelText(toWx(decibelLabel(slider->GetValue())));

    slider->Bind(wxEVT_SLIDER, [this, key, readout](wxCommandEvent& event) {
        const int scaled = event.GetInt();
        readout->SetLabelText(toWx(decibelLabel(scaled)));
        settings_.setRawValue(key, std::to_string(static_cast<double>(scaled) / kEqScale));
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

void EqualizerPanel::flatten() {
    // Every band explicitly, and that is the one real difference from the Qt
    // version. There, setValue() emitted valueChanged, so writing the sliders
    // wrote the settings and told the engine as a side effect. wxSlider::SetValue
    // raises **no** event -- programmatic changes never do in wx -- so relying on
    // it would leave the sliders flat and the audio unchanged.
    for (std::size_t i = 0; i < sliders_.size(); ++i) {
        sliders_[i]->SetValue(0);
        readouts_[i]->SetLabelText(toWx(decibelLabel(0)));
        settings_.setRawValue(keys_[i], "0");
        settingChanged.publish(keys_[i]);
    }
}

}  // namespace xpcog::app
