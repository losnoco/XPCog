#include "SpeedPanel.hpp"

#include "Localization.hpp"
#include "SpeedCurve.hpp"
#include "Text.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include <string_view>

namespace xpcog::app {

SpeedPanel::SpeedPanel(wxWindow* parent, Settings& settings)
    : wxPanel(parent, wxID_ANY), settings_(settings) {
    auto* column = new wxBoxSizer(wxVERTICAL);

    // Three columns -- caption, slider, readout -- with the slider column the
    // one that grows, so a pane dragged wider gives the travel to the control
    // that can use it rather than to the gap after the number.
    auto* grid = new wxFlexGridSizer(3, FromDIP(6), FromDIP(8));
    grid->AddGrowableCol(1);

    const auto slider = [&](double initial) {
        return new wxSlider(this, wxID_ANY, sliderFromSpeed(initial), 0,
                            kSpeedSliderMax, wxDefaultPosition,
                            FromDIP(wxSize(180, -1)));
    };

    pitch_      = slider(settings_.Pitch());
    tempo_      = slider(settings_.Tempo());
    pitchValue_ = new wxStaticText(this, wxID_ANY, "");
    tempoValue_ = new wxStaticText(this, wxID_ANY, "");

    // The preferences pane's labels verbatim, so the two controls read the same
    // and the catalogue already has them.
    const auto row = [&](const wxString& text, wxSlider* control,
                         wxStaticText* value) {
        auto* caption = new wxStaticText(this, wxID_ANY, text);
        grid->Add(caption, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
        grid->Add(value, 0, wxALIGN_CENTER_VERTICAL);
        return caption;
    };
    pitchLabel_ = row(_("Pitch"), pitch_, pitchValue_);
    tempoLabel_ = row(_("Tempo"), tempo_, tempoValue_);
    column->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    lock_  = new wxCheckBox(this, wxID_ANY, _("Lock pitch and tempo together"));
    reset_ = new wxButton(this, wxID_ANY, trUtf8("Reset to 1.00\xC3\x97"));
    // The rest of Pitch & Tempo -- the engine and its knobs -- without making
    // somebody find the pane. "Preferences" rather than "Settings" because that
    // is what this application calls that window everywhere else, and the
    // ellipsis is the convention for a control that opens one.
    auto* settingsButton =
        new wxButton(this, wxID_ANY, trUtf8("Preferences\xE2\x80\xA6"));
    buttons->Add(lock_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    buttons->AddStretchSpacer();
    buttons->Add(reset_, 0, wxRIGHT, FromDIP(6));
    buttons->Add(settingsButton, 0);
    column->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    // Said here rather than left to be discovered: the engine ships disabled,
    // so out of the box both sliders write a setting nothing reads. The
    // Settings button beside it is the way out, which is why this is a sentence
    // and not a disabled-looking pane.
    note_ = new wxStaticText(
        this, wxID_ANY, _("No engine is chosen, so these do nothing yet."));
    note_->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    column->Add(note_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    pitch_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        write("pitch", snapSpeed(speedFromSlider(pitch_->GetValue())));
    });
    tempo_->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        write("tempo", snapSpeed(speedFromSlider(tempo_->GetValue())));
    });
    lock_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setRawValue("speedLock", event.IsChecked() ? "true" : "false");
        settingChanged.publish("speedLock");
    });
    reset_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        write("pitch", 1.0);
        write("tempo", 1.0);
    });
    settingsButton->Bind(wxEVT_BUTTON,
                         [this](wxCommandEvent&) { settingsRequested.publish(); });

    SetSizer(column);
    refresh();
}

void SpeedPanel::showValue(wxStaticText* label, double ratio) {
    // Through FromUTF8, as every label with a multiplication sign must be: a
    // char* handed straight to wxString goes through the ANSI code page on
    // Windows.
    label->SetLabel(wxString::Format(wxString::FromUTF8("%.2f\xC3\x97"), ratio));
}

void SpeedPanel::write(const char* key, double ratio) {
    settings_.setRawValue(key, wxString::FromDouble(ratio).utf8_string());
    settingChanged.publish(key);

    // Cog's speed lock is the UI writing both keys, not the engine linking them
    // (SpeedButton.m pressLock:) -- ported as-is, so an imported plist's lock
    // behaves identically. Under varispeed there is nothing to lock: the engine
    // moves pitch with tempo itself, and the pitch slider is not on screen.
    const bool varispeed = settings_.RubberbandEngine() == "varispeed";
    if (settings_.SpeedLock() && !varispeed) {
        const char* other = (std::string_view{key} == "pitch") ? "tempo" : "pitch";
        settings_.setRawValue(other, wxString::FromDouble(ratio).utf8_string());
        settingChanged.publish(other);
    }
    refresh();
}

void SpeedPanel::refresh() {
    const std::string engine    = settings_.RubberbandEngine();
    const bool        disabled  = engine == "disabled";
    // Varispeed resamples, as a record player does: pitch *is* tempo under it,
    // so a second slider would be a duplicate of the first and a lock would
    // have nothing to join. The preferences pane hides both for the same
    // reason (buildPitchTempoPane's refresh()).
    const bool        varispeed = engine == "varispeed";

    const double pitch = settings_.Pitch();
    const double tempo = settings_.Tempo();
    pitch_->SetValue(sliderFromSpeed(pitch));
    tempo_->SetValue(sliderFromSpeed(tempo));
    showValue(pitchValue_, pitch);
    showValue(tempoValue_, tempo);
    lock_->SetValue(settings_.SpeedLock());

    bool changed = false;
    for (wxWindow* window : {static_cast<wxWindow*>(pitchLabel_),
                             static_cast<wxWindow*>(pitch_),
                             static_cast<wxWindow*>(pitchValue_),
                             static_cast<wxWindow*>(lock_)}) {
        changed |= window->Show(!varispeed);
    }
    changed |= note_->Show(disabled);

    if (changed) {
        // A row arriving or leaving changes how tall the pane wants to be, and
        // the sizer only knows once it is asked again.
        Layout();
    }
}

}  // namespace xpcog::app
