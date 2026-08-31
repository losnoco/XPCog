#include "SpeedPopup.hpp"

#include "Localization.hpp"
#include "SpeedCurve.hpp"
#include "Text.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include <string_view>

namespace xpcog::app {
namespace {

// A visible edge, because a popup has no title bar and no shadow to separate it
// from what it covers. And on MSW, the flag that exists precisely because the
// default popup style leaves controls inside it unresponsive -- sliders being
// exactly the case it is for.
constexpr int kPopupStyle =
#ifdef __WXMSW__
    wxBORDER_SIMPLE | wxPU_CONTAINS_CONTROLS;
#else
    wxBORDER_SIMPLE;
#endif

}  // namespace

SpeedPopup::SpeedPopup(wxWindow* parent, Settings& settings)
    : wxPopupTransientWindow(parent, kPopupStyle), settings_(settings) {
    auto* panel  = new wxPanel(this);
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto* grid   = new wxFlexGridSizer(3, FromDIP(6), FromDIP(8));
    grid->AddGrowableCol(1);

    const auto slider = [&](double initial) {
        return new wxSlider(panel, wxID_ANY, sliderFromSpeed(initial), 0,
                            kSpeedSliderMax, wxDefaultPosition,
                            FromDIP(wxSize(190, -1)));
    };

    pitch_      = slider(settings_.Pitch());
    tempo_      = slider(settings_.Tempo());
    pitchValue_ = new wxStaticText(panel, wxID_ANY, "");
    tempoValue_ = new wxStaticText(panel, wxID_ANY, "");

    // The pane's labels verbatim, so the two controls read the same and the
    // catalogue already has them.
    const auto row = [&](const wxString& label, wxSlider* control,
                         wxStaticText* value) {
        grid->Add(new wxStaticText(panel, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
        grid->Add(value, 0, wxALIGN_CENTER_VERTICAL);
    };
    row(_("Pitch"), pitch_, pitchValue_);
    row(_("Tempo"), tempo_, tempoValue_);
    column->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));

    lock_ = new wxCheckBox(panel, wxID_ANY, _("Lock pitch and tempo together"));
    column->Add(lock_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    auto* reset = new wxButton(panel, wxID_ANY, trUtf8("Reset to 1.00\xC3\x97"));
    column->Add(reset, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    // Said here rather than left to be discovered: the engine ships disabled,
    // so out of the box both sliders write a setting that nothing reads. The
    // pane hides the sliders in that case; hiding them here would leave a popup
    // with nothing in it, which explains less than a sentence does.
    disabled_ = new wxStaticText(
        panel, wxID_ANY,
        _("Choose a Pitch & Tempo engine in Preferences to hear these."));
    disabled_->SetForegroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    column->Add(disabled_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

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
    reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        write("pitch", 1.0);
        write("tempo", 1.0);
    });

    auto* outer = new wxBoxSizer(wxVERTICAL);
    panel->SetSizer(column);
    outer->Add(panel, 1, wxEXPAND);
    SetSizerAndFit(outer);

    refresh();
}

void SpeedPopup::showValue(wxStaticText* label, double ratio) {
    // Through FromUTF8, as every label with a multiplication sign must be: a
    // char* handed straight to wxString goes through the ANSI code page on
    // Windows.
    label->SetLabel(wxString::Format(wxString::FromUTF8("%.2f\xC3\x97"), ratio));
}

void SpeedPopup::write(const char* key, double ratio) {
    settings_.setRawValue(key, wxString::FromDouble(ratio).utf8_string());
    settingChanged.publish(key);

    // Cog's speed lock is the UI writing both keys, not the engine linking them
    // (SpeedButton.m pressLock:) -- ported as-is, so an imported plist's lock
    // behaves identically.
    if (settings_.SpeedLock()) {
        const char* other = (std::string_view{key} == "pitch") ? "tempo" : "pitch";
        settings_.setRawValue(other, wxString::FromDouble(ratio).utf8_string());
        settingChanged.publish(other);
    }
    refresh();
}

void SpeedPopup::refresh() {
    const double pitch = settings_.Pitch();
    const double tempo = settings_.Tempo();
    pitch_->SetValue(sliderFromSpeed(pitch));
    tempo_->SetValue(sliderFromSpeed(tempo));
    showValue(pitchValue_, pitch);
    showValue(tempoValue_, tempo);
    lock_->SetValue(settings_.SpeedLock());

    if (disabled_->Show(settings_.RubberbandEngine() == "disabled")) {
        // Only when it actually changed: the popup is sized to its contents and
        // the sentence arriving or leaving changes how tall that is.
        Layout();
        Fit();
    }
}

void SpeedPopup::popupUnder(wxWindow* anchor) {
    refresh();
    // The anchor's own screen rect, not a point already offset by its height.
    // wxPopupWindowBase::Position (src/common/popupcmn.cpp) opens at
    // `ptOrigin + size` and, when the popup will not fit below, flips to
    // `ptOrigin - sizeSelf` -- so it needs the size of the thing being avoided
    // in order to land *above* the button rather than on top of it. Passing a
    // pre-offset origin and a zero size looks identical until the window is near
    // the bottom of the screen, and then the popup covers its own button.
    Position(anchor->GetScreenPosition(), anchor->GetSize());
    Popup();
}

}  // namespace xpcog::app
