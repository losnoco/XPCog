#include "PreferencesDialog.hpp"

#include "LastFmAccount.hpp"
#include "Localization.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/platform/CrashReporter.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/listbox.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/taskbar.h>
#include <wx/textctrl.h>
#include <wx/translation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::app {
namespace {

/// A setting whose values are a closed set, so it deserves a named list rather
/// than a text box. The **values** are Cog's stored values, unchanged; only the
/// labels beside them are language. Translating a value would write a Spanish
/// word into a settings file Cog is expected to be able to read.
struct Choice {
    const char* value;
    const char* label;  ///< marked with wxTRANSLATE; looked up by choice()
};

constexpr std::array kVolumeScalingChoices = {
    Choice{"none", wxTRANSLATE("None")},
    Choice{"volumeScale", wxTRANSLATE("Volume tag")},
    Choice{"soundcheck", wxTRANSLATE("iTunes Sound Check")},
    Choice{"trackGain", wxTRANSLATE("Track gain")},
    Choice{"trackGainWithPeak", wxTRANSLATE("Track gain, peak-limited")},
    Choice{"albumGain", wxTRANSLATE("Album gain")},
    Choice{"albumGainWithPeak", wxTRANSLATE("Album gain, peak-limited")},
};

constexpr std::array kResamplingChoices = {
    Choice{"quick", wxTRANSLATE("Quick")}, Choice{"low", wxTRANSLATE("Low")},   Choice{"medium", wxTRANSLATE("Medium")},
    Choice{"high", wxTRANSLATE("High")},   Choice{"best", wxTRANSLATE("Best")},
};

// The stretch engines and the Rubber Band option vocabularies, values and
// defaults from Cog's Rubber Band pane (Preferences/Panes/RubberbandPaneView
// .swift). `varispeed` is ours: Cog has no resampling speed control.
constexpr std::array kStretchEngineChoices = {
    Choice{"disabled", wxTRANSLATE("Disabled")},
    Choice{"varispeed", wxTRANSLATE("Varispeed \xE2\x80\x94 resample, pitch follows tempo")},
    Choice{"signalsmith", wxTRANSLATE("Signalsmith Stretch")},
    Choice{"faster", wxTRANSLATE("Rubber Band \xE2\x80\x94 Faster")},
    Choice{"finer", wxTRANSLATE("Rubber Band \xE2\x80\x94 Finer")},
};
constexpr std::array kRubberTransientsChoices = {
    Choice{"crisp", wxTRANSLATE("Crisp")}, Choice{"mixed", wxTRANSLATE("Mixed")}, Choice{"smooth", wxTRANSLATE("Smooth")},
};
constexpr std::array kRubberDetectorChoices = {
    Choice{"compound", wxTRANSLATE("Compound")},
    Choice{"percussive", wxTRANSLATE("Percussive")},
    Choice{"soft", wxTRANSLATE("Soft")},
};
constexpr std::array kRubberPhaseChoices = {
    Choice{"laminar", wxTRANSLATE("Laminar")},
    Choice{"independent", wxTRANSLATE("Independent")},
};
constexpr std::array kRubberWindowChoices = {
    Choice{"standard", wxTRANSLATE("Standard")}, Choice{"short", wxTRANSLATE("Short")}, Choice{"long", wxTRANSLATE("Long")},
};
constexpr std::array kRubberSmoothingChoices = {
    Choice{"off", wxTRANSLATE("Off")},
    Choice{"on", wxTRANSLATE("On")},
};
constexpr std::array kRubberFormantChoices = {
    Choice{"shifted", wxTRANSLATE("Shifted")},
    Choice{"preserved", wxTRANSLATE("Preserved")},
};
constexpr std::array kRubberPitchChoices = {
    Choice{"highspeed", wxTRANSLATE("High speed")},
    Choice{"highquality", wxTRANSLATE("High quality")},
    Choice{"highconsistency", wxTRANSLATE("High consistency")},
};
constexpr std::array kRubberChannelsChoices = {
    Choice{"apart", wxTRANSLATE("Apart")},
    Choice{"together", wxTRANSLATE("Together")},
};

/// Cog's slider curve (PlaybackController.m speedScale): a slider position in
/// 0..100 becomes a ratio in 0.2..5.0, quadratically, so the octave around 1.0
/// gets most of the travel.
[[nodiscard]] double speedFromSlider(int position) {
    const double x = static_cast<double>(position);
    return ((x * x) * (5.0 - 0.2) / 10000.0) + 0.2;
}
[[nodiscard]] int sliderFromSpeed(double ratio) {
    const double clamped = std::clamp(ratio, 0.2, 5.0);
    return static_cast<int>(std::lround(std::sqrt((clamped - 0.2) * 10000.0 / (5.0 - 0.2))));
}

/// The synthesisers `midiPlugin` can name, in Cog's own spelling.
///
/// Nuked OPL3 twice over -- id's DMX driver, once per instrument bank, and
/// Nuke.YKT's General MIDI one -- and then an emulated Roland. The OPL labels are
/// the drivers' own bank names (vendor/nuked-opl3), spelled out here rather than
/// read back from them so the dialog does not have to construct a synthesiser to
/// draw a list. See docs/MIDI.md.
constexpr std::array kMidiSynthChoices = {
    Choice{"DOOM0", wxTRANSLATE("OPL3 \xE2\x80\x94 DMX default")},
    Choice{"DOOM1", wxTRANSLATE("OPL3 \xE2\x80\x94 DMX Doom")},
    Choice{"DOOM2", wxTRANSLATE("OPL3 \xE2\x80\x94 DMX Doom II")},
    Choice{"DOOM3", wxTRANSLATE("OPL3 \xE2\x80\x94 DMX Raptor")},
    Choice{"DOOM4", wxTRANSLATE("OPL3 \xE2\x80\x94 DMX Strife")},
    Choice{"DOOM5", wxTRANSLATE("OPL3 \xE2\x80\x94 DMXOPL")},
    Choice{"OPL3W0", wxTRANSLATE("OPL3 \xE2\x80\x94 General MIDI")},
    Choice{"Spessa", wxTRANSLATE("SoundFont \xE2\x80\x94 SpessaSynth")},
    Choice{"NukeSc55", wxTRANSLATE("Roland SC-55")},
};

[[nodiscard]] bool isTrue(const std::string& text) {
    // Cog's plist stores YES/NO; Settings accepts both those and true/false.
    return text == "1" || text == "true" || text == "YES";
}

[[nodiscard]] int toInt(const std::string& text) {
    try {
        return text.empty() ? 0 : std::stoi(text);
    } catch (const std::exception&) {
        return 0;
    }
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

/// Settings that already have a hand-written row in one of the panes above. The
/// generated pane skips them, so each setting is edited in exactly one place --
/// otherwise a curated list and a raw text box for the same key sit two clicks
/// apart, disagreeing about what the value should look like.
constexpr std::array kCuratedKeys = {
    // Playlist
    "alwaysStopAfterCurrent", "readCueSheetsInFolders", "readPlaylistsInFolders",
    "selectionFollowsPlayback", "resumePlaybackOnStartup",
    // Owned by a control outside this dialog, and listed here so that Advanced
    // does not offer a second one that disagrees with it. `volume` is the
    // transport slider; `repeat` and `shuffle` are the Order menu's radio
    // groups; `panelFollowMode` is View -> Panels Follow. All four are state a
    // gesture sets, not preferences someone comes here to type.
    "volume", "repeat", "shuffle", "panelFollowMode",
    // Output
    "volumeScaling", "resampling", "enableHDCD", "halveDSDVolume", "outputDeviceId",
    "outputDeviceName", "exclusiveOutput", "enableFSurround", "enableFading",
    "suspendOutputOnPause",
    // MIDI
    "midiPlugin", "midiRomPath", "soundFontPath", "synthSampleRate",
    "synthDefaultSeconds", "synthDefaultFadeSeconds", "synthDefaultLoopCount",
    // Appearance
    //
    //
    // widgetStyle is listed even though no pane draws a row for it any more. The
    // toolkit has no style engine -- see docs/WXPORT.md -- so the key is dead
    // rather than merely unused, and a dead key belongs in Advanced even less
    // than it belongs in Appearance. Kept in settings.def so a settings file that
    // has travelled from a Qt build keeps its value rather than losing it.
    //
    // Both stay listed on macOS, where the pane itself is not built. Curated is
    // the right side of this list for them there too: closeToTray is answered by
    // the platform and widgetStyle is dead, and neither belongs in Advanced,
    // where a raw editable row would offer control that does not exist.
    "widgetStyle", "closeToTray",
    // General. `language` has a picker there, and Advanced must not offer a
    // second one: its generated row would be a free-text box for a value that
    // has exactly three valid answers, one of which is the empty string.
    "language",
    // Spectrum
    "spectrumBarColor", "spectrumDotColor", "spectrumFreqMode", "spectrumFloorDb",
    "spectrumShowPeaks",
    // General
    "sentryConsented", "httpStreamingBufferSize",
    // Appearance
    "floatingMiniWindow",
    // Notifications
    "notifications.enable", "notifications.show-album-art",
};

/// Not settings at all, but internal state that happens to live in the same
/// store. Shown, because the generated pane's whole point is that nothing is
/// hidden, but not editable: settingsSchemaVersion drives
/// Settings::applyMigrations(), so typing into it makes migrations re-run or be
/// skipped, and nothing about a spin box suggests that. UserDefaultURLsKey is the
/// Open URL history -- a newline-separated list the dialog maintains, where a
/// hand edit can only produce entries that will not parse. sentryAskedConsent
/// records that the prompt has been shown; it is the answer next to it on General
/// that decides anything, and a checkbox here that re-armed a one-time dialog
/// would read as a second consent switch.
/// The rest are the session's own record of itself rather than anything asked
/// for: which mode the window was in, how playback was left and where, whether
/// the tray notice has been shown, and the pre-split `outputDevice` a settings
/// file may still carry. Editing any of them changes what the *last* session is
/// remembered to have done, which is not a preference.
constexpr std::array kInternalKeys = {"settingsSchemaVersion", "UserDefaultURLsKey",
                                      "sentryAskedConsent",    "lastPlaybackStatus",
                                      "miniMode",              "trayHideAnnounced",
                                      "outputDevice"};

[[nodiscard]] bool contains(std::span<const char* const> keys, std::string_view key) {
    return std::any_of(keys.begin(), keys.end(),
                       [key](const char* candidate) { return key == candidate; });
}

[[nodiscard]] bool hasCuratedRow(std::string_view key) {
    // Every equaliser key -- eqPreamp and the 31 bands -- has a slider of its own
    // in the equaliser panel, so they are matched by prefix rather than listed
    // twice. 32 raw spin boxes in Advanced would be a second, worse equaliser.
    if (key.starts_with("eq")) {
        return true;
    }
    // And the two the preset row owns, which do not share that prefix because
    // they are Cog's names. `GraphicEQpreset` is the worse of the two to leave
    // here: it is an index into a list this dialog cannot show, so a spin box
    // would offer a number with no way to find out which preset it means.
    if (key.starts_with("GraphicEQ")) {
        return true;
    }
    return contains(kCuratedKeys, key);
}

/// A two-column form: a caption and a control, with the control column growing.
/// wxFlexGridSizer is what QFormLayout was.
[[nodiscard]] wxFlexGridSizer* makeForm(int gap) {
    auto* form = new wxFlexGridSizer(2, gap, gap * 2);
    form->AddGrowableCol(1, 1);
    return form;
}

/// Every pane scrolls, in both directions.
///
/// Only Advanced used to, and only vertically. The others were fixed panels
/// inside a fixed dialog, so a pane whose rows came to more than the dialog
/// simply lost the far side of itself -- silently, and worse in a language whose
/// sentences run longer than the English they were sized against, which is most
/// of them. MIDI's form wants 437 DIP in English and 581 in Spanish with the
/// SC-55's ROM row showing; the pane it is given is about 470.
///
/// Horizontally matters as much as vertically and for a reason worth writing
/// down, because it is not what the scroll rate appears to say.
/// wxScrollHelperBase::ScrollLayout lays the sizer out at the *virtual* size --
/// except in a direction where no scrollbar is shown, where it substitutes the
/// client size. So with horizontal scrolling off, a form whose minimum is wider
/// than the pane is handed the pane's width, lays its rows out at their own
/// minimums anyway, and runs off the right edge with nothing to reach it. A
/// scrollbar is not a nicety here: it is the difference between overflow being
/// visible and being cut.
///
/// It stays out of the way when it is not needed. FitInside() sets the virtual
/// size to the form's minimum, so a form that fits shows no horizontal bar --
/// and with no bar shown, ScrollLayout goes back to the client width and the
/// controls stretch to fill the pane exactly as before.
using Pane = wxScrolled<wxPanel>;

[[nodiscard]] Pane* makePane(wxWindow* parent) {
    auto* pane = new Pane(parent, wxID_ANY);
    pane->SetScrollRate(pane->FromDIP(8), pane->FromDIP(8));
    return pane;
}

/// How wide a paragraph in `pane` may be.
///
/// The vertical scrollbar's width is subtracted whether or not it is showing,
/// and that is the whole trick: text that wrapped to the full client width would
/// grow tall enough to need a scrollbar, which narrows the client, which wraps
/// it taller still. Reserving the space unconditionally means the wrap width
/// does not move when the scrollbar appears, so it cannot oscillate.
[[nodiscard]] wxWindow* finishPane(Pane* pane, wxSizer* form) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    // Proportion zero, not one. A growable item is stretched to the window's
    // height, which for a scrolled window is the *visible* height -- so the form
    // would be squashed back into the pane it is meant to be able to overflow,
    // and FitInside() would then compute a virtual size that never exceeded the
    // client one. Nothing would ever scroll.
    layout->Add(form, 0, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    pane->FitInside();
    return pane;
}

/// A paragraph of explanation that re-wraps as the dialog changes width.
///
/// Two things have to be true at once, and the second is what the first attempt
/// at this got wrong.
///
/// **It has to be re-wrappable.** wxStaticText::Wrap() inserts the breaks into
/// the label itself, so it cannot simply be called again -- a second call wraps
/// the already-wrapped text and the paragraph creeps narrower with every resize
/// until it is one word per line. Keeping the original and re-setting it before
/// each wrap is what makes it repeatable.
///
/// **And it has to wrap to the width it will actually be given**, which is the
/// *second column's*, not the pane's: a note sits in a two-column form beside a
/// caption column that is a third of the pane wide. Wrapping to the pane's width
/// -- which is what the fixed `FromDIP(440)` here amounted to as well -- writes
/// lines about a third too long for the cell they are drawn in, and a
/// wxStaticText does not scroll or ellipsize: the ends are simply cut off. That
/// is the clipped help text this dialog shipped with.
///
/// So the width comes from this window's own size event, which is the width the
/// sizer just handed it. The obvious objection is that wrapping changes this
/// window's best size and would therefore change the width it is next given --
/// a loop. `SetMinSize` is what breaks it: a note contributes almost nothing to
/// its column's minimum width, so the column is sized by the controls above it
/// and the number arriving in the size event does not move when the text
/// re-wraps. It converges in one extra event, and the guard below stops even
/// that from doing any work.
class NoteText : public wxStaticText {
public:
    /// `quiet` greys the text, which is right for a pane explaining itself and
    /// wrong for the Last.fm pane's status line -- that one is the answer to
    /// what the listener just did.
    NoteText(Pane* pane, const wxString& text, bool quiet = true)
        : wxStaticText(pane, wxID_ANY, text), original_(text) {
        if (quiet) {
            Enable(false);
        }
        pinMinimumWidth();
        // Bound to its *own* size event rather than to the pane's. The pane
        // outlives nothing here -- it destroys this window on its way out -- but
        // a handler registered on the parent is not removed when the child dies,
        // and a size event arriving in that window is a call through a destroyed
        // object. Self-binding cannot get that wrong.
        Bind(wxEVT_SIZE, &NoteText::onResized, this);
    }

    /// Replaces the text, keeping the wrap. For a note whose words change.
    void setText(const wxString& text) {
        original_ = text;
        SetLabel(original_);
        if (wrappedAt_ > 0) {
            Wrap(wrappedAt_);
        }
        pinMinimumWidth();
        relayout();
    }

private:
    /// Height from the wrapped text, width from almost nothing.
    ///
    /// wxSizerItem asks for the effective minimum size, which takes each
    /// component from the min size where it is set and from the best size where
    /// it is -1 -- so this keeps the paragraph's real height, which the row needs
    /// to be tall enough for, and throws away its width, which is the half that
    /// would otherwise widen the column and feed back into the wrap.
    void pinMinimumWidth() { SetMinSize(wxSize(FromDIP(40), -1)); }

    void onResized(wxSizeEvent& event) {
        event.Skip();

        const int cell = event.GetSize().GetWidth();
        // A window that has not been laid out yet reports nonsense.
        if (cell < FromDIP(80)) {
            return;
        }

        // Capped at what is actually on screen, not just at the cell. The pane
        // scrolls horizontally when a *control* row is wider than it, and the
        // form is then laid out at that wider size -- so a paragraph filling its
        // cell would be a paragraph you have to scroll sideways to read, which
        // is not what a paragraph is for. It wraps into the visible width
        // instead and the scrollbar stays the controls' problem.
        //
        // The offset is the unscrolled one, so how far the pane happens to be
        // scrolled cannot change where the text wraps.
        auto*     pane    = static_cast<Pane*>(GetParent());
        const int left    = pane->CalcUnscrolledPosition(GetPosition()).x;
        const int visible = pane->GetClientSize().GetWidth() - left - FromDIP(8);
        const int room    = std::max(FromDIP(80), std::min(cell, visible));

        // A width that has not moved is work with nothing to show for it.
        if (room == wrappedAt_) {
            return;
        }
        wrappedAt_ = room;
        SetLabel(original_);
        Wrap(room);
        pinMinimumWidth();
        relayout();
    }

    void relayout() {
        auto* pane = static_cast<Pane*>(GetParent());
        pane->Layout();
        // The virtual size follows the form's new height, or a paragraph that
        // grew from two lines to five would be as clipped at the bottom as it
        // used to be at the side.
        pane->FitInside();
    }

    wxString original_;
    int      wrappedAt_ = -1;
};

/// Adds curated rows to one pane's form.
///
/// A struct rather than a lambda per pane because there are four panes wanting
/// the same handful of row kinds, and four copies of "read the raw value, write
/// it back, announce it" is four chances for one of them to forget the
/// announcement. It reports changes through a callback rather than publishing the
/// dialog's own signal, which is not something a helper should be reaching into.
/// A handle onto one added form row, for the panes that show rows only while
/// some other setting holds a particular value.
///
/// Hiding both cells is what collapses the row: wxFlexGridSizer keeps every
/// item in its grid cell rather than reflowing around hidden ones, and gives a
/// row whose items are all hidden a height of -1 (wxFlexGridSizer::CalcMin in
/// wx's own sizer.cpp), so the rows around it close up without their label and
/// control pairing ever shifting.
struct FormRow {
    wxWindow* label   = nullptr;
    wxWindow* control = nullptr;  ///< the row is one control...
    wxSizer*  holder  = nullptr;  ///< ...or a sizer of them (file and path rows)

    void show(bool visible) const {
        if (label != nullptr) {
            label->Show(visible);
        }
        if (control != nullptr) {
            control->Show(visible);
        }
        if (holder != nullptr) {
            holder->ShowItems(visible);
        }
    }
};

class RowBuilder : public wxClientData {
public:
    using Announce = std::function<void(const char*)>;

    RowBuilder(Settings& settings, Pane* pane, wxFlexGridSizer* form,
               Announce announce)
        : settings_(&settings), pane_(pane), form_(form), announce_(std::move(announce)) {}

    /// `onChange` runs after the setting is written and announced, with the
    /// newly stored value. It exists for the two panes whose rows appear and
    /// disappear under another row's value; binding a second handler on the
    /// control from outside would work only by accident of wx's dispatch
    /// order, so the hook is part of the row instead.
    FormRow choice(const wxString& label, const char* key,
                   std::span<const Choice> choices,
                   std::function<void(const std::string&)> onChange = {}) const {
        wxArrayString items;
        for (const Choice& option : choices) {
            items.Add(trUtf8(option.label));
        }
        auto* box = new wxChoice(pane_, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);

        // A stored value this build does not offer -- a settings file carried
        // over from a macOS Cog naming an AudioUnit, say -- selects the first
        // entry, which is the same fallback the decoder reading it applies.
        const std::string current = settings_->rawValue(key);
        int               index   = 0;
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (current == choices[i].value) {
                index = static_cast<int>(i);
                break;
            }
        }
        box->SetSelection(index);

        // The value strings are copied into the handler: `choices` is a span over
        // a constexpr array, which outlives everything, but relying on that from
        // a lambda that escapes is the kind of thing that stops being true later.
        std::vector<std::string> values;
        values.reserve(choices.size());
        for (const Choice& option : choices) {
            values.emplace_back(option.value);
        }

        box->Bind(wxEVT_CHOICE, [settings = settings_, announce = announce_, key,
                                 values, onChange = std::move(onChange)](wxCommandEvent& event) {
            const auto selected = static_cast<std::size_t>(event.GetSelection());
            if (selected < values.size()) {
                settings->setRawValue(key, values[selected]);
                announce(key);
                if (onChange) {
                    onChange(values[selected]);
                }
            }
        });
        return add(label, box);
    }

    /// The row's control is the checkbox, for the callers that grey it out or
    /// hide its row.
    FormRow toggle(const wxString& label, const char* key,
                   const wxString& hint = wxString{}) const {
        auto* box = new wxCheckBox(pane_, wxID_ANY, label);
        box->SetValue(isTrue(settings_->rawValue(key)));
        if (!hint.IsEmpty()) {
            box->SetToolTip(hint);
        }
        box->Bind(wxEVT_CHECKBOX,
                  [settings = settings_, announce = announce_, key](wxCommandEvent& event) {
                      settings->setRawValue(key, event.IsChecked() ? "true" : "false");
                      announce(key);
                  });
        return add("", box);
    }

    /// A link out of the application, laid out where a note() would be.
    ///
    /// Exists for one row -- the privacy policy beside the crash-reporting
    /// switch -- and it is worth a method rather than a hand-built control there
    /// because "here is what you are agreeing to" is not something to leave as
    /// text somebody has to retype into a browser.
    void link(const wxString& label, std::string_view url) const {
        auto* control = new wxHyperlinkCtrl(pane_, wxID_ANY, label,
                                            wxString::FromUTF8(std::string{url}));
        form_->AddSpacer(0);
        form_->Add(control, 0, wxTOP, pane_->FromDIP(2));
    }

    void number(const wxString& label, const char* key, int minimum,
                int maximum) const {
        auto* box = new wxSpinCtrl(pane_, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, wxSP_ARROW_KEYS, minimum, maximum,
                                   toInt(settings_->rawValue(key)));
        box->Bind(wxEVT_SPINCTRL,
                  [settings = settings_, announce = announce_, key](wxSpinEvent& event) {
                      settings->setRawValue(key, std::to_string(event.GetPosition()));
                      announce(key);
                  });
        add(label, box);
    }

    void seconds(const wxString& label, const char* key, double maximum) const {
        auto* box = new wxSpinCtrlDouble(pane_, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 0.0, maximum,
                                         toDouble(settings_->rawValue(key)), 0.1);
        box->SetDigits(1);
        box->Bind(wxEVT_SPINCTRLDOUBLE, [settings = settings_, announce = announce_,
                                         key](wxSpinDoubleEvent& event) {
            settings->setRawValue(key, std::to_string(event.GetValue()));
            announce(key);
        });
        add(label, box);
    }

    /// One file, chosen or typed. For a setting that names a single file and
    /// nothing else -- a SoundFont bank -- where the folder button a path() row
    /// carries would only offer something that cannot be right.
    FormRow file(const wxString& label, const char* key,
                 const wxString& filter) const {
        auto* edit = makeEdit(key);

        auto* browse = new wxButton(pane_, wxID_ANY, _("Choose..."));
        browse->Bind(wxEVT_BUTTON, [this, edit, key, label, filter](wxCommandEvent&) {
            apply(edit, key,
                  wxFileSelector(label, wxEmptyString, wxEmptyString, wxEmptyString,
                                 filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST, pane_));
        });

        return addWithButtons(label, edit, {browse});
    }

    /// A path the listener has to supply, with the two ways of choosing one.
    ///
    /// Two buttons rather than one because the thing being named may be either a
    /// folder or a file, and no file dialog on any platform offers both at once.
    /// The text box accepts a typed or pasted path either way.
    FormRow path(const wxString& label, const char* key,
                 const wxString& archiveFilter) const {
        auto* edit = makeEdit(key);

        auto* folder = new wxButton(pane_, wxID_ANY, _("Folder..."));
        folder->Bind(wxEVT_BUTTON, [this, edit, key, label](wxCommandEvent&) {
            apply(edit, key,
                  wxDirSelector(label, wxEmptyString, wxDD_DEFAULT_STYLE,
                                wxDefaultPosition, pane_));
        });

        auto* archive = new wxButton(pane_, wxID_ANY, _("Archive..."));
        archive->Bind(wxEVT_BUTTON,
                      [this, edit, key, label, archiveFilter](wxCommandEvent&) {
                          apply(edit, key,
                                wxFileSelector(label, wxEmptyString, wxEmptyString,
                                               wxEmptyString, archiveFilter,
                                               wxFD_OPEN | wxFD_FILE_MUST_EXIST,
                                               pane_));
                      });

        return addWithButtons(label, edit, {folder, archive});
    }

    FormRow note(const wxString& text) const {
        auto* label = new NoteText(pane_, text);
        // An empty label rather than a spacer in the first cell: a spacer item
        // counts as shown whatever the text beside it does, and a "hidden"
        // note would otherwise leave its grid row open by a gap's height.
        auto* pad = new wxStaticText(pane_, wxID_ANY, "");
        form_->Add(pad, 0);
        form_->Add(label, 1, wxEXPAND | wxTOP, pane_->FromDIP(6));
        return FormRow{pad, label, nullptr};
    }

    FormRow add(const wxString& label, wxWindow* control) const {
        auto* text = new wxStaticText(pane_, wxID_ANY, label);
        form_->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        form_->Add(control, 1, wxEXPAND);
        return FormRow{text, control, nullptr};
    }

private:
    /// A text box that writes its setting when it loses focus or takes Return.
    [[nodiscard]] wxTextCtrl* makeEdit(const char* key) const {
        auto* edit = new wxTextCtrl(pane_, wxID_ANY, toWx(settings_->rawValue(key)),
                                    wxDefaultPosition, wxDefaultSize,
                                    wxTE_PROCESS_ENTER);
        // A path box takes every spare pixel in its row -- it is the item with
        // the proportion -- so its *minimum* has no business deciding how wide
        // the form is. wxTextCtrl's default best width is around 140 DIP, and
        // between that and two buttons whose labels grow with the language, the
        // MIDI pane's minimum came to 581 DIP against a pane of 470.
        edit->SetMinSize(wxSize(pane_->FromDIP(60), -1));
        auto* settings = settings_;
        auto  announce = announce_;

        const auto commit = [settings, announce, edit, key] {
            settings->setRawValue(key, toUtf8(edit->GetValue()));
            announce(key);
        };
        // Both, because wx raises neither for the other's case: Enter does not
        // move focus and losing focus does not send a text-enter event.
        edit->Bind(wxEVT_TEXT_ENTER, [commit](wxCommandEvent&) { commit(); });
        edit->Bind(wxEVT_KILL_FOCUS, [commit](wxFocusEvent& event) {
            event.Skip();
            commit();
        });
        return edit;
    }

    /// What a chooser button does with whatever the dialog returned. Empty is the
    /// cancelled dialog, and leaves the setting alone.
    void apply(wxTextCtrl* edit, const char* key, const wxString& chosen) const {
        if (chosen.IsEmpty()) {
            return;
        }
        edit->SetValue(wxFileName(chosen).GetFullPath());
        // Written through here rather than left to the text box's own handlers:
        // SetValue raises wxEVT_TEXT, but not the enter or kill-focus events the
        // commit is bound to, so nothing else would notice.
        settings_->setRawValue(key, toUtf8(edit->GetValue()));
        announce_(key);
    }

    FormRow addWithButtons(const wxString& label, wxTextCtrl* edit,
                           std::initializer_list<wxButton*> buttons) const {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(edit, 1, wxALIGN_CENTER_VERTICAL);
        for (wxButton* button : buttons) {
            row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pane_->FromDIP(4));
        }
        auto* text = new wxStaticText(pane_, wxID_ANY, label);
        form_->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        form_->Add(row, 1, wxEXPAND);
        return FormRow{text, nullptr, row};
    }

    // Held by pointer, and every handler captures the pointer rather than `this`:
    // a RowBuilder is a local in the function that builds a pane and is gone by
    // the time anyone clicks anything, while the settings object and the callback
    // both outlive the dialog.
    Settings*        settings_;
    Pane*            pane_;
    wxFlexGridSizer* form_;
    Announce         announce_;
};

}  // namespace

PreferencesDialog::PreferencesDialog(wxWindow* parent, Settings& settings,
                                     LastFmAccount* account, Scrobbler* scrobbler)
    : wxDialog(parent, wxID_ANY, _("Preferences"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      settings_(settings),
      account_(account),
      scrobbler_(scrobbler) {
    SetSize(FromDIP(wxSize(700, 480)));
    // A floor, now that the panes scroll. Without one, "it fits because it
    // scrolls" is true all the way down to a dialog with room for half a row --
    // scrolling is there so a long pane is reachable, not so the window can be
    // made useless. The width is the narrowest the two-column form reads at with
    // the category list beside it; the height is about six rows.
    SetMinSize(FromDIP(wxSize(520, 320)));

    // A list beside a stack of pages, which is exactly what the Qt version built
    // from a QListWidget and a QStackedWidget.
    //
    // wxListbook is the obvious fit and was what this used first. Its sidebar is
    // a wxListCtrl, and wxMSW's wxListCtrl reports no best size of its own, so
    // wxBookCtrlBase::GetControllerSize() falls back through GetBestSize() to
    // wxControl's default of about a hundred pixels -- whatever the category
    // names happen to be. That is how "Playlist" and "Appearance" came out as
    // "Playli..." and "App...". wxListBox measures its widest item to decide its
    // width, on every platform, so the sidebar is the right size by construction
    // rather than by a hand-tuned number that would go wrong again at another
    // DPI or in another language.
    auto* categories = new wxListBox(this, wxID_ANY);
    auto* book       = new wxSimplebook(this, wxID_ANY);

    const auto page = [&](wxWindow* pane, const wxString& name) {
        book->AddPage(pane, name);
        categories->Append(name);
    };

    // Cog's order, with its unported panes taken out rather than reshuffled.
    // General sits after Output because that is where Cog has it -- fourth, after
    // Playlist, Hot Keys and Output -- rather than first, where the name suggests.
    page(buildPlaylistPane(book), _("Playlist"));
    page(buildOutputPane(book), _("Output"));
    // Plain "&": the name lands in a wxListBox, which draws text verbatim
    // rather than eating ampersands the way menus and buttons do.
    page(buildPitchTempoPane(book), _("Pitch & Tempo"));
    page(buildGeneralPane(book), _("General"));
    page(buildNotificationsPane(book), _("Notifications"));
    // Cog has a Last.fm pane of its own and puts it last, after Appearance and
    // MIDI. It goes beside Notifications here because that is what it is: the
    // other place this program reports what it is playing to something outside
    // itself. Skipped entirely when the application did not pass one in.
    if (account_ != nullptr && scrobbler_ != nullptr) {
        page(buildLastFmPane(book), "Last.fm");  // a proper noun
    }
    // Absent on macOS. Its only control is the close-to-tray checkbox, which is
    // already Windows and Linux only -- macOS closes to the Dock unconditionally,
    // by platform convention rather than by preference -- so what remains there is
    // a category whose page is one greyed-out paragraph. That reads as a screen
    // that failed to load, and the paragraph says nothing a macOS user needs
    // telling: following the system appearance is what every application on the
    // platform does.
#ifndef __WXOSX__
    page(buildAppearancePane(book), _("Appearance"));
#endif
    page(buildMidiPane(book), "MIDI");  // an acronym, the same in every language
    page(buildSpectrumPane(book), _("Spectrum"));
    page(buildAdvancedPane(book), _("Advanced"));

    categories->SetSelection(0);
    book->ChangeSelection(0);
    categories->Bind(wxEVT_LISTBOX, [book](wxCommandEvent& event) {
        if (event.GetSelection() >= 0) {
            book->ChangeSelection(static_cast<std::size_t>(event.GetSelection()));
        }
    });

    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    columns->Add(categories, 0, wxEXPAND | wxRIGHT, FromDIP(8));
    columns->Add(book, 1, wxEXPAND);

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(columns, 1, wxEXPAND | wxALL, FromDIP(8));
    if (wxSizer* buttons = CreateStdDialogButtonSizer(wxCLOSE); buttons != nullptr) {
        layout->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    SetSizer(layout);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
}

PreferencesDialog::~PreferencesDialog() {
    // An attempt still waiting on a browser has nothing left to report to, and
    // leaving it running would keep polling Last.fm for three minutes after the
    // window closed. The credential half of a reply already in flight still
    // lands; see paneAlive_.
    if (account_ != nullptr) {
        account_->cancelConnect();
    }
}

std::function<void(const char*)> PreferencesDialog::changeNotifier() {
    return [this](const char* key) { settingChanged.publish(key); };
}

wxWindow* PreferencesDialog::buildPlaylistPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    row->toggle(_("Stop after every track"), "alwaysStopAfterCurrent");
    row->toggle(_("Follow the playing track in the playlist"),
                "selectionFollowsPlayback",
                _("Move the selection to each track as it starts."));
    row->toggle(_("Resume playback on startup"), "resumePlaybackOnStartup",
                _("Continue the last track from where it stopped. The track is "
                  "selected either way."));
    // Both off by default, as in Cog, and for the reason Cog has: a folder
    // holding album.cue or its own .m3u beside the audio otherwise adds every
    // track twice -- once through the container and once as the file under it.
    row->toggle(_("Read cue sheets when adding folders"), "readCueSheetsInFolders");
    row->toggle(_("Read playlists when adding folders"), "readPlaylistsInFolders");

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildGeneralPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // The language, first, and on General rather than Appearance -- Appearance
    // is not built on macOS at all, and this is the one row on it that every
    // platform needs. Cog has no equivalent: macOS carries a per-application
    // language preference of its own, and Windows does not.
    //
    // By hand rather than through RowBuilder::choice: the rows come from what
    // the build compiled in rather than from a constexpr table, and the first of
    // them is not a language.
    wxArrayString            languageNames;
    std::vector<std::string> languageCodes;
    for (const LanguageOption& option : availableLanguages()) {
        languageNames.Add(option.code.empty() ? _("Follow the system")
                                              : toWx(option.name));
        languageCodes.push_back(option.code);
    }

    auto* languageBox =
        new wxChoice(pane, wxID_ANY, wxDefaultPosition, wxDefaultSize, languageNames);
    const std::string chosenLanguage = settings_.Language();
    languageBox->SetSelection(0);
    for (std::size_t i = 0; i < languageCodes.size(); ++i) {
        if (languageCodes[i] == chosenLanguage) {
            languageBox->SetSelection(static_cast<int>(i));
            break;
        }
    }
    languageBox->Bind(wxEVT_CHOICE, [this, languageCodes](wxCommandEvent& event) {
        const auto index = static_cast<std::size_t>(event.GetSelection());
        if (index >= languageCodes.size()) {
            return;
        }
        settings_.setLanguage(languageCodes[index]);
        settingChanged.publish("language");
        // Flushed now rather than at quit. This is a setting whose only effect
        // is on the *next* launch, and somebody who changes it is quite likely
        // to restart the player rather than close the window tidily first.
        settings_.sync();
    });
    row->add(_("Language"), languageBox);
    row->note(_("Restart XPCog to apply."));

    // Cog keeps the streaming buffer on General too, under a Network heading
    // (GeneralPaneView.swift:120-131). One row does not need a heading.
    row->number(_("Streaming buffer (bytes)"), "httpStreamingBufferSize", 65536,
                134217728);
    row->note(_("How much of an internet radio stream is read ahead. Raise it for "
                "a slow or distant station."));

    // Cog's label, word for word (Preferences/Panes/GeneralPaneView.swift:133).
    // "Usage data" is not padding: session tracking is on, so a launch and a
    // clean exit are reported as well as a crash, and a label saying only "crash
    // reports" would be describing less than what is sent.
    auto* box = static_cast<wxCheckBox*>(
        row->toggle(_("Send crash reports and usage data"), "sentryConsented").control);

    if (platform::crashReportingAvailable()) {
        row->note(_("Nothing is collected or sent while this is off."));
        row->link(_("Privacy policy"), platform::kPrivacyPolicyUrl);
    } else {
        // Shown rather than hidden, and greyed rather than lying. A build
        // configured without XPCOG_WITH_SENTRY has no reporter to start, and a
        // checkbox that ticks and does nothing is worse than one that explains
        // itself -- particularly this checkbox, where what it appears to promise
        // runs in the direction of sending more.
        box->Enable(false);
        box->SetValue(false);
        row->note(_("Crash reporting is not included in this build."));
    }

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildNotificationsPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // Cog's two, with Cog's labels and Cog's defaults -- both on.
    row->toggle(_("Enable notifications"), "notifications.enable");
    row->toggle(_("Show album art"), "notifications.show-album-art");
    row->note(_("Shown as each track starts. Focus Assist or Do Not Disturb can "
                "hold notifications back."));

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildOutputPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // The device list is read once, when this pane is built. Enumerating spins up
    // the backend's context, and a picker that re-enumerated on every repaint
    // would do that while the listener is dragging a slider next to it.
    wxArrayString            names;
    std::vector<std::string> ids;
    names.Add(_("System default"));
    ids.emplace_back();

    const std::string chosenId = settings_.rawValue("outputDeviceId");
    for (const DeviceInfo& device : enumerateOutputDevices()) {
        // Named rather than left to be guessed at: "System default" and the
        // device it currently resolves to are different rows, and which one is
        // chosen matters when headphones are plugged in later.
        names.Add(device.isDefault
                      ? wxString::Format(_("%s (current default)"), toWx(device.name))
                      : toWx(device.name));
        ids.push_back(device.id);
    }

    // A device that is not here right now is still the choice: the engine falls
    // back to the default until it returns, and the setting is left alone. So the
    // row is shown rather than silently reset to "System default".
    int chosenRow = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == chosenId) {
            chosenRow = static_cast<int>(i);
            break;
        }
    }
    if (chosenRow == 0 && !chosenId.empty()) {
        const std::string name = settings_.rawValue("outputDeviceName");
        names.Add(wxString::Format(_("%s (not connected)"),
                                   toWx(name.empty() ? chosenId : name)));
        ids.push_back(chosenId);
        chosenRow = static_cast<int>(ids.size()) - 1;
    }

    auto* deviceBox =
        new wxChoice(pane, wxID_ANY, wxDefaultPosition, wxDefaultSize, names);
    deviceBox->SetSelection(chosenRow);
    deviceBox->Bind(wxEVT_CHOICE, [this, ids, names](wxCommandEvent& event) {
        const auto index = static_cast<std::size_t>(event.GetSelection());
        if (index >= ids.size()) {
            return;
        }
        settings_.setRawValue("outputDeviceId", ids[index]);
        // The name travels with it, as the fallback match for a device that comes
        // back under a new id.
        settings_.setRawValue("outputDeviceName",
                              ids[index].empty() ? std::string{}
                                                 : toUtf8(names[index]));
        settingChanged.publish("outputDeviceId");
    });
    row->add(_("Output device"), deviceBox);

    row->toggle(_("Play exclusively"), "exclusiveOutput",
                _("Use the file's own rate and format instead of the system "
                  "mixer's. Other applications cannot play while this is active. "
                  "Falls back to sharing if the device is unavailable."));

    row->choice(_("Volume scaling"), "volumeScaling", kVolumeScalingChoices);
    row->choice(_("Resampler quality"), "resampling", kResamplingChoices);

    row->toggle(_("Decode HDCD"), "enableHDCD",
                _("Applies to 16-bit 44.1 kHz stereo only. Files without HDCD "
                  "codes are unaffected."));
    row->toggle(_("Halve DSD volume"), "halveDSDVolume",
                trUtf8("DSD is converted with a filter whose gain puts half modulation "
                       "\xE2\x80\x94 as loud as most SACDs go \xE2\x80\x94 at full "
                  "scale. Turn on if a loud SACD rip clips."));
    row->toggle(_("Upmix stereo to surround"), "enableFSurround",
                _("Uses FreeSurround. Takes effect when the device is next "
                  "opened."));
    row->toggle(_("Fade on seek and stop"), "enableFading");
    // On, the device is handed back while paused; off, it is held open and fed
    // silence. Off is what someone chooses when reacquiring costs more than it
    // saves -- an exclusive device another application may take in the gap.
    row->toggle(_("Release the device while paused"), "suspendOutputOnPause",
                _("Let other applications use the device while playback is paused. "
                  "Turn off to keep an exclusive device reserved."));

    row->note(_("Volume scaling and resampler quality apply from the next track. "
                "Changing the device moves playback across with a brief gap."));

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildPitchTempoPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // Every row below the engine picker exists only under some engines, so the
    // handles live on the heap where the picker's onChange can reach them --
    // they are filled in as the rows are built, which is after the picker, and
    // read only when a human changes the selection, which is later still.
    struct Rows {
        FormRow pitch, tempo, lock, reset;
        FormRow transients, detector, phase, window, smoothing, formant,
            pitchMode, channels, note;
        wxChoice*                windowBox = nullptr;
        std::vector<std::string> windowValues;
    };
    auto rows = std::make_shared<Rows>();

    // The Window picker's items depend on the engine -- R3 has no Long window,
    // and Cog both removes the item and rewrites a stored "long" to standard
    // when someone switches to Finer. Rebuilding the items is why this row is
    // built by hand further down rather than through RowBuilder.
    const auto rebuildWindow = [this, rows](bool finer) {
        std::string current = settings_.RubberbandWindow();
        if (finer && current == "long") {
            current = "standard";
            settings_.setRawValue("rubberbandWindow", current);
            settingChanged.publish("rubberbandWindow");
        }
        rows->windowBox->Clear();
        rows->windowValues.clear();
        int selected = 0;
        for (const Choice& option : kRubberWindowChoices) {
            if (finer && std::string_view{option.value} == "long") {
                continue;
            }
            if (current == option.value) {
                selected = static_cast<int>(rows->windowValues.size());
            }
            rows->windowBox->Append(trUtf8(option.label));
            rows->windowValues.emplace_back(option.value);
        }
        rows->windowBox->SetSelection(selected);
    };

    // What each engine shows. The sliders exist for every engine but Disabled;
    // varispeed hides the pitch slider and the lock because under it pitch
    // *is* tempo, and the Rubber Band rows exist only for the two Rubber Band
    // engines -- with the four R2-only rows gone under Finer, which is Cog's
    // own pane behaviour (RubberbandPaneView.swift's isR3 checks).
    const auto refresh = [pane, rows, rebuildWindow](const std::string& engine) {
        const bool any       = engine != "disabled";
        const bool varispeed = engine == "varispeed";
        const bool finer     = engine == "finer";
        const bool rubber    = engine == "faster" || finer;

        rows->pitch.show(any && !varispeed);
        rows->tempo.show(any);
        rows->lock.show(any && !varispeed);
        rows->reset.show(any);

        rows->transients.show(rubber && !finer);
        rows->detector.show(rubber && !finer);
        rows->phase.show(rubber && !finer);
        rows->window.show(rubber);
        rows->smoothing.show(rubber && !finer);
        rows->formant.show(rubber);
        rows->pitchMode.show(rubber);
        rows->channels.show(rubber);
        rows->note.show(varispeed);

        if (rubber) {
            rebuildWindow(finer);
        }
        pane->Layout();
        // Rows arriving and leaving changes how tall the form is, and on a
        // scrolled pane the scrollbar only knows about it if the virtual size
        // is recomputed. Without this, switching to a Rubber Band engine adds
        // eight rows that cannot be scrolled to.
        pane->FitInside();
    };

    row->choice(_("Engine"), "rubberbandEngine", kStretchEngineChoices, refresh);

    // The two speed sliders, on the pane beside the engine that obeys them
    // rather than in the transport bar where Cog keeps its pair -- the bar
    // here is already full, and a control that does nothing until an engine is
    // chosen belongs next to that choice. Cog's quadratic curve and Cog's
    // range, with the value spelled out beside each slider because the curve
    // makes the position unreadable.
    auto* pitchSlider = new wxSlider(pane, wxID_ANY, sliderFromSpeed(settings_.Pitch()),
                                     0, 100, wxDefaultPosition,
                                     pane->FromDIP(wxSize(220, -1)));
    auto* pitchValue  = new wxStaticText(pane, wxID_ANY, "");
    auto* tempoSlider = new wxSlider(pane, wxID_ANY, sliderFromSpeed(settings_.Tempo()),
                                     0, 100, wxDefaultPosition,
                                     pane->FromDIP(wxSize(220, -1)));
    auto* tempoValue  = new wxStaticText(pane, wxID_ANY, "");

    const auto showValue = [](wxStaticText* text, double ratio) {
        // Through FromUTF8, as every label with a multiplication sign must be:
        // a char* handed straight to wxString goes through the ANSI code page
        // on Windows, which is where the first draft's mojibake came from.
        text->SetLabel(wxString::Format(wxString::FromUTF8("%.2f\xC3\x97"), ratio));
    };
    showValue(pitchValue, settings_.Pitch());
    showValue(tempoValue, settings_.Tempo());

    // Cog's snapSpeeds: the slider cannot otherwise land back on exactly 1.0,
    // and "very nearly unstretched" runs the whole engine for nothing audible.
    const auto snapped = [](double ratio) {
        return (std::fabs(ratio - 1.0) < 0.01) ? 1.0 : ratio;
    };

    const auto applySpeed = [this, showValue, snapped](
                                const char* key, wxSlider* slider,
                                wxStaticText* value, const char* otherKey,
                                wxSlider* otherSlider, wxStaticText* otherValue) {
        const double ratio = snapped(speedFromSlider(slider->GetValue()));
        showValue(value, ratio);
        settings_.setRawValue(key, wxString::FromDouble(ratio).utf8_string());
        settingChanged.publish(key);
        // Cog's speed lock is the UI writing both keys, not the engine linking
        // them -- ported as-is, so an imported plist's lock behaves identically.
        if (settings_.SpeedLock()) {
            otherSlider->SetValue(sliderFromSpeed(ratio));
            showValue(otherValue, ratio);
            settings_.setRawValue(otherKey, wxString::FromDouble(ratio).utf8_string());
            settingChanged.publish(otherKey);
        }
    };

    pitchSlider->Bind(wxEVT_SLIDER, [=, this](wxCommandEvent&) {
        applySpeed("pitch", pitchSlider, pitchValue, "tempo", tempoSlider, tempoValue);
    });
    tempoSlider->Bind(wxEVT_SLIDER, [=, this](wxCommandEvent&) {
        applySpeed("tempo", tempoSlider, tempoValue, "pitch", pitchSlider, pitchValue);
    });

    const auto speedRow = [&](const wxString& label, wxSlider* slider,
                              wxStaticText* value) {
        auto* text = new wxStaticText(pane, wxID_ANY, label);
        form->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        auto* holder = new wxBoxSizer(wxHORIZONTAL);
        holder->Add(slider, 1, wxALIGN_CENTER_VERTICAL);
        holder->Add(value, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pane->FromDIP(6));
        form->Add(holder, 1, wxEXPAND);
        return FormRow{text, nullptr, holder};
    };
    rows->pitch = speedRow(_("Pitch"), pitchSlider, pitchValue);
    rows->tempo = speedRow(_("Tempo"), tempoSlider, tempoValue);

    rows->lock = row->toggle(_("Lock pitch and tempo together"), "speedLock",
                             _("Moving either slider moves both, which is what a "
                               "record player's speed control does."));

    auto* reset = new wxButton(pane, wxID_ANY, trUtf8("Reset to 1.00\xC3\x97"));
    reset->Bind(wxEVT_BUTTON, [=, this](wxCommandEvent&) {
        pitchSlider->SetValue(sliderFromSpeed(1.0));
        tempoSlider->SetValue(sliderFromSpeed(1.0));
        showValue(pitchValue, 1.0);
        showValue(tempoValue, 1.0);
        settings_.setRawValue("pitch", "1");
        settings_.setRawValue("tempo", "1");
        settingChanged.publish("pitch");
        settingChanged.publish("tempo");
    });
    rows->reset = row->add("", reset);

    // Rubber Band's own knobs, labels and vocabulary from Cog's pane -- shown
    // only while a Rubber Band engine is chosen, see refresh() above.
    rows->transients = row->choice(_("Transients"), "rubberbandTransients",
                                   kRubberTransientsChoices);
    rows->detector =
        row->choice(_("Detector"), "rubberbandDetector", kRubberDetectorChoices);
    rows->phase = row->choice(_("Phase"), "rubberbandPhase", kRubberPhaseChoices);

    // By hand rather than through RowBuilder: its items change with the
    // engine, which rebuildWindow() above owns.
    rows->windowBox = new wxChoice(pane, wxID_ANY);
    rows->windowBox->Bind(wxEVT_CHOICE, [this, rows](wxCommandEvent& event) {
        const auto selected = static_cast<std::size_t>(event.GetSelection());
        if (selected < rows->windowValues.size()) {
            settings_.setRawValue("rubberbandWindow", rows->windowValues[selected]);
            settingChanged.publish("rubberbandWindow");
        }
    });
    rows->window = row->add(_("Window"), rows->windowBox);

    rows->smoothing =
        row->choice(_("Smoothing"), "rubberbandSmoothing", kRubberSmoothingChoices);
    rows->formant =
        row->choice(_("Formant"), "rubberbandFormant", kRubberFormantChoices);
    rows->pitchMode =
        row->choice(_("Pitch mode"), "rubberbandPitch", kRubberPitchChoices);
    rows->channels =
        row->choice(_("Channels"), "rubberbandChannels", kRubberChannelsChoices);

    rows->note = row->note(_("Varispeed resamples, as a record player would: one "
                             "tempo slider, and the pitch follows it."));

    refresh(settings_.RubberbandEngine());

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildMidiPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // The bank rows belong to one synthesiser each: a SoundFont means nothing
    // to the SC-55 and ROMs mean nothing to SpessaSynth, so each row is shown
    // only while its owner is chosen -- with its note, which is most of what
    // the note has to say.
    struct Rows {
        FormRow soundFont, roms, spessaNote, sc55Note;
    };
    auto rows = std::make_shared<Rows>();

    const auto refresh = [pane, rows](const std::string& synth) {
        const bool spessa = synth == "Spessa";
        const bool sc55   = synth == "NukeSc55";
        rows->soundFont.show(spessa);
        rows->spessaNote.show(spessa);
        rows->roms.show(sc55);
        rows->sc55Note.show(sc55);
        pane->Layout();
        pane->FitInside();
    };

    row->choice(_("Synthesiser"), "midiPlugin", kMidiSynthChoices, refresh);
    rows->soundFont = row->file(
        _("SoundFont"), "soundFontPath",
        _("SoundFont banks") + "|*.sf2;*.sf3;*.sf2pack;*.dls;*.sflist;*.json|" +
            _("All Files") + "|*.*");
    rows->roms = row->path(_("SC-55 ROMs"), "midiRomPath",
                           _("Archives") + "|*.zip;*.rar;*.7z|" + _("All Files") +
                               "|*.*");

    // Cog's clamps, and its labels. The sample rate is the rate a synthesiser
    // renders at rather than the rate the file plays at -- there is no such thing
    // as the second one for a score.
    row->number(_("Sample rate (Hz)"), "synthSampleRate", 8000, 192000);
    row->seconds(_("Default play time (s)"), "synthDefaultSeconds", 3600.0);
    row->seconds(_("Default fade time (s)"), "synthDefaultFadeSeconds", 60.0);
    row->number(_("Default loop count"), "synthDefaultLoopCount", 0, 10);

    rows->spessaNote =
        row->note(_("SpessaSynth needs a bank: any .sf2, .sf3 or .dls. Files that "
                    "carry their own bank use that instead."));
    rows->sc55Note =
        row->note(_("The SC-55 needs its five ROM files, which are not supplied. "
                    "Choose the folder or the archive they came in; they are "
                    "recognised by content, so nothing needs renaming. Without "
                    "them, MIDI plays on the OPL3. It also ignores the sample "
                    "rate and always renders at its own."));
    row->note(_("These apply to every synthesised format, not only MIDI."));

    refresh(settings_.MidiPlugin());

    return finishPane(pane, form);
}

// Not built on macOS at all -- see the page list in the constructor. The guard is
// here rather than only around the caller so the pane's one control keeps its
// single `#ifndef`, instead of an empty function surviving for no one to call.
#ifndef __WXOSX__
wxWindow* PreferencesDialog::buildAppearancePane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // No style picker. The Qt build offered windows11 / windowsvista / Fusion
    // because Qt draws its own controls and can therefore draw them several ways;
    // wxWidgets uses the platform's, which is the point of the move, and has no
    // equivalent. The `widgetStyle` key is kept in settings.def so a settings file
    // that travelled from a Qt build keeps its value, and kCuratedKeys keeps it
    // out of Advanced, where a dead key would be worse.

    // The reason this whole pane is absent on macOS: the question this asks does
    // not arise there. Closing the window hides it and leaves XPCog running --
    // unconditionally, because that is the platform's convention rather than a
    // preference -- and there is no tray icon to hide to in any case. A checkbox
    // offering to choose something already chosen is the same fault as one that
    // does nothing.
    //
    // Named for the thing rather than described. "Closing the window keeps
    // XPCog running" was a sentence about a consequence, which reads as an
    // explanation and is therefore worse at being a label: close-to-tray is what
    // this behaviour is called, it is what somebody arrives looking for, and it
    // is what the tooltip and the notice both already say.
    auto* closeToTray = new wxCheckBox(pane, wxID_ANY, _("Close to tray"));
    closeToTray->SetValue(settings_.CloseToTray());
    closeToTray->SetToolTip(
        _("Closing the window leaves XPCog running in the notification area "
          "instead of quitting."));
    // Offered only where there is somewhere to hide to. Without a notification
    // area this would hide the window with no way to bring it back, so the
    // setting is ignored at the call site *and* disabled here -- a checkbox that
    // does nothing is worse than an absent one, and this at least says why.
    if (!wxTaskBarIcon::IsAvailable()) {
        closeToTray->Enable(false);
        closeToTray->SetToolTip(_("This session has no notification area to keep "
                                  "XPCog in."));
    }
    closeToTray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setCloseToTray(event.IsChecked());
        settingChanged.publish("closeToTray");
    });
    row->add("", closeToTray);

    // The mini player's own control is a button on the mini player, which is only
    // reachable once you are in it. Here as well, so it can be set beforehand.
    row->toggle(_("Keep the mini player on top"), "floatingMiniWindow");

    // No note about following the system appearance. It said that XPCog has no
    // theme of its own, which is an answer to a question nobody standing in front
    // of two checkboxes has asked -- and it read as an apology for a pane that
    // does not need one.

    return finishPane(pane, form);
}
#endif  // !__WXOSX__

wxWindow* PreferencesDialog::buildSpectrumPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // Bands. Cog's two analyser modes, stored in its own key: false is the note
    // scale, true the even spacing. A list rather than a checkbox because
    // "Frequency mode: off" says nothing about what you get instead.
    wxArrayString bandModes;
    bandModes.Add(_("Musical notes (one bar per semitone)"));
    bandModes.Add(_("Even frequency spacing"));
    auto* bands = new wxChoice(pane, wxID_ANY, wxDefaultPosition, wxDefaultSize, bandModes);
    bands->SetSelection(settings_.SpectrumFreqMode() ? 1 : 0);
    bands->Bind(wxEVT_CHOICE, [this](wxCommandEvent& event) {
        settings_.setSpectrumFreqMode(event.GetSelection() == 1);
        settingChanged.publish("spectrumFreqMode");
    });
    row->add(_("Bands"), bands);

    // wxColourPickerCtrl is a real colour button -- what the Qt version built
    // from a QPushButton, a generated swatch pixmap and QColorDialog.
    //
    // An unparseable stored value -- notably an imported Cog colour, which is an
    // archived NSColor rather than a string -- shows as the setting's own default
    // rather than as black. A black bar on a near-black background looks like the
    // display is broken.
    const auto colourRow = [&](const wxString& label, const std::string& stored,
                               std::function<void(const std::string&)> store) {
        wxColour initial;
        if (!initial.Set(toWx(stored))) {
            initial.Set("#ff8000");
        }
        auto* picker = new wxColourPickerCtrl(pane, wxID_ANY, initial);
        picker->Bind(wxEVT_COLOURPICKER_CHANGED,
                     [store = std::move(store)](wxColourPickerEvent& event) {
                         // Lower case and six digits, which is the form the
                         // setting documents; wx spells it upper case.
                         std::string hex =
                             toUtf8(event.GetColour().GetAsString(wxC2S_HTML_SYNTAX));
                         std::transform(hex.begin(), hex.end(), hex.begin(),
                                        [](unsigned char c) {
                                            return static_cast<char>(std::tolower(c));
                                        });
                         store(hex);
                     });
        row->add(label, picker);
    };

    colourRow(_("Bar colour"), settings_.SpectrumBarColor(), [this](const std::string& hex) {
        settings_.setSpectrumBarColor(hex);
        settingChanged.publish("spectrumBarColor");
    });
    colourRow(_("Peak colour"), settings_.SpectrumDotColor(), [this](const std::string& hex) {
        settings_.setSpectrumDotColor(hex);
        settingChanged.publish("spectrumDotColor");
    });

    auto* peaks = new wxCheckBox(pane, wxID_ANY, _("Show peak markers"));
    peaks->SetValue(settings_.SpectrumShowPeaks());
    peaks->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setSpectrumShowPeaks(event.IsChecked());
        settingChanged.publish("spectrumShowPeaks");
    });
    row->add("", peaks);

    // The floor. Cog fixes this at -80; the range here is wide enough to be useful
    // at both ends and stops short of zero, where there would be nothing to draw.
    auto* floorDb = new wxSpinCtrl(pane, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, wxSP_ARROW_KEYS, -120, -20,
                                   static_cast<int>(std::lround(settings_.SpectrumFloorDb())));
    floorDb->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent& event) {
        settings_.setSpectrumFloorDb(static_cast<double>(event.GetPosition()));
        settingChanged.publish("spectrumFloorDb");
    });
    row->add(_("Quietest level shown (dB)"), floorDb);

    // One sentence, and only because it says what the display *means*. The
    // sentence that followed it explained why the lowest bars move together,
    // which is defending the analysis to someone who was choosing a colour.
    row->note(_("Bars sit on semitones from C0, so the display lines up with the "
                "notes being played."));

    return finishPane(pane, form);
}

wxWindow* PreferencesDialog::buildAdvancedPane(wxWindow* parent) {
    // Scrolled, because the list grows with every milestone and a fixed pane
    // would quietly start clipping.
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(4));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // Generated from settings.def. A setting added there shows up here without any
    // edit to this file, which is what stops a setting existing in the engine but
    // being unreachable from the UI. As settings graduate to a hand-written row in
    // one of the panes above, they leave this list via kCuratedKeys rather than
    // being duplicated by it.
    for (const Settings::Desc& descriptor : Settings::all()) {
        if (hasCuratedRow(descriptor.key)) {
            continue;
        }

        const auto key   = std::string{descriptor.key};
        const auto label = std::string{descriptor.ident};
        const auto value = settings_.rawValue(key);

        wxWindow* editor = nullptr;
        if (descriptor.type == "bool") {
            auto* box = new wxCheckBox(pane, wxID_ANY, wxEmptyString);
            box->SetValue(isTrue(value));
            box->Bind(wxEVT_CHECKBOX, [this, key](wxCommandEvent& event) {
                settings_.setRawValue(key, event.IsChecked() ? "true" : "false");
                settingChanged.publish(key);
            });
            editor = box;
        } else if (descriptor.type == "int") {
            auto* box = new wxSpinCtrl(pane, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                       wxDefaultSize, wxSP_ARROW_KEYS, -1000000, 1000000,
                                       toInt(value));
            box->Bind(wxEVT_SPINCTRL, [this, key](wxSpinEvent& event) {
                settings_.setRawValue(key, std::to_string(event.GetPosition()));
                settingChanged.publish(key);
            });
            editor = box;
        } else if (descriptor.type == "double") {
            auto* box = new wxSpinCtrlDouble(pane, wxID_ANY, wxEmptyString,
                                             wxDefaultPosition, wxDefaultSize,
                                             wxSP_ARROW_KEYS, -1000000.0, 1000000.0,
                                             toDouble(value), 0.001);
            box->SetDigits(3);
            box->Bind(wxEVT_SPINCTRLDOUBLE, [this, key](wxSpinDoubleEvent& event) {
                settings_.setRawValue(key, std::to_string(event.GetValue()));
                settingChanged.publish(key);
            });
            editor = box;
        } else {
            auto* edit = new wxTextCtrl(pane, wxID_ANY, toWx(value), wxDefaultPosition,
                                        wxDefaultSize, wxTE_PROCESS_ENTER);
            const auto commit = [this, edit, key] {
                settings_.setRawValue(key, toUtf8(edit->GetValue()));
                settingChanged.publish(key);
            };
            edit->Bind(wxEVT_TEXT_ENTER, [commit](wxCommandEvent&) { commit(); });
            edit->Bind(wxEVT_KILL_FOCUS, [commit](wxFocusEvent& event) {
                event.Skip();
                commit();
            });
            editor = edit;
        }

        if (contains(kInternalKeys, descriptor.key)) {
            editor->Enable(false);
            editor->SetToolTip(
                _("Maintained automatically, and not meant to be edited."));
        }
        // The setting's C++ identifier, deliberately untranslated: this pane
        // exists so that nothing is unreachable, and the name it shows has to be
        // the one settings.def uses or a bug report naming it means nothing.
        row->add(toWx(label), editor);
    }

    row->note(_("The greyed rows are what XPCog remembers about the last session, "
                "not settings."));

    return finishPane(pane, form);
}

// --- Last.fm --------------------------------------------------------------

wxWindow* PreferencesDialog::buildLastFmPane(wxWindow* parent) {
    auto* pane = makePane(parent);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    auto* enable = static_cast<wxCheckBox*>(
        row->toggle(_("Scrobble to Last.fm"), "enableAudioScrobbler").control);

    // The status line and the buttons are rebuilt rather than recreated, so the
    // pane has one place that decides what state it is in. Everything below is a
    // closure over these three.
    // A NoteText rather than a plain wxStaticText: this is the pane's longest
    // paragraph and it sits in the same second column the notes do, so it wants
    // the same wrap. Not greyed, though -- it is the answer to what the listener
    // just did rather than a footnote about the pane.
    auto* status  = new NoteText(pane, wxEmptyString, false);
    auto* connect = new wxButton(pane, wxID_ANY, _("Connect..."));
    auto* cancel  = new wxButton(pane, wxID_ANY, _("Cancel"));
    auto* forget  = new wxButton(pane, wxID_ANY, _("Disconnect"));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(connect, 0, wxRIGHT, pane->FromDIP(6));
    buttons->Add(cancel, 0, wxRIGHT, pane->FromDIP(6));
    buttons->Add(forget, 0);

    form->AddSpacer(0);
    form->Add(status, 1, wxEXPAND | wxTOP | wxBOTTOM, pane->FromDIP(6));
    form->AddSpacer(0);
    form->Add(buttons, 0, wxBOTTOM, pane->FromDIP(6));

    // A token proving this pane is still on screen.
    //
    // The connect flow answers on a worker and is marshalled back through
    // wxTheApp -- deliberately, because the *credential* half of the reply has to
    // land even if this dialog has been closed in the meantime: a listener who
    // authorises and then closes Preferences has still authorised. But that same
    // choice means the reply is not cancelled when the dialog dies, so anything
    // touching a widget has to check first. `wxEvtHandler::CallAfter` on the
    // dialog itself would drop the event and take the credential with it, which
    // is the worse of the two failures.
    // Held by this dialog, so it expires exactly when the dialog does.
    paneAlive_               = std::make_shared<int>(0);
    const std::weak_ptr<int> token = paneAlive_;

    // Captured by the handlers below. `refresh` is the only thing that decides
    // what is shown, so there is no way for two paths to disagree about it.
    const auto refresh = [this, token, status, connect, cancel, forget, enable] {
        if (token.expired()) {
            return;
        }
        const bool built = account_->usable();
        wxString   storeProblem;
        const bool store   = LastFmAccount::storeAvailable(&storeProblem);
        const bool working = account_->connecting();
        const auto session = scrobbler_->session();

        enable->Enable(built && store);
        connect->Show(!session.connected() && !working);
        cancel->Show(working);
        forget->Show(session.connected() && !working);
        connect->Enable(built && store);

        // Built up and handed over once, rather than written to the control and
        // then read back to append to. Reading it back stopped being an option
        // when the control started wrapping: GetLabel() returns the text *with*
        // the line breaks wrapping put in, so appending to it and wrapping again
        // would fold the paragraph a little narrower on every refresh.
        wxString text;
        if (!built) {
            // Greyed and explained rather than hidden, which is how the
            // crash-reporting checkbox handles a build without Sentry. A control
            // that does nothing is worse than one that says why.
            enable->SetValue(false);
            text = account_->unavailableReason();
        } else if (!store) {
            text = storeProblem.empty()
                       ? wxString(_("The system password store is not available, "
                                    "so a Last.fm session cannot be kept."))
                       : storeProblem;
        } else if (working) {
            text = _("Waiting for you to allow access in your browser...");
        } else if (session.connected()) {
            text = wxString::Format(_("Connected as %s."),
                                    wxString::FromUTF8(session.username));
        } else {
            text = _("Not connected. Connecting opens Last.fm in your browser; "
                     "XPCog never sees your password.");
        }

        const std::size_t waiting = scrobbler_->pending();
        if (waiting > 0) {
            // Worth saying, because the alternative is a listener concluding
            // that the plays were lost. They were not; they are on disk.
            text += "\n";
            text += wxString::Format(wxPLURAL("%zu play waiting to be sent.",
                                              "%zu plays waiting to be sent.",
                                              static_cast<unsigned>(waiting)),
                                     waiting);
        }

        // Wraps and re-lays the pane out itself.
        status->setText(text);
    };

    connect->Bind(wxEVT_BUTTON, [this, refresh, token, status](wxCommandEvent&) {
        // Captured as plain pointers rather than through `this`: both outlive
        // this dialog -- MainFrame owns them -- so a reply that arrives after
        // Preferences closes still applies the session it was granted.
        Scrobbler* const scrobbler = scrobbler_;
        Settings* const  settings  = &settings_;

        LastFmAccount::ConnectHandlers handlers;
        handlers.awaitingAuthorization = [refresh](const wxString&) { refresh(); };
        handlers.connected = [refresh, scrobbler, settings](
                                 const Scrobbler::Session& session) {
            scrobbler->setSession(session);
            // Connecting is the act that makes the switch mean something, so it
            // turns it on -- rather than leaving a listener who just authorised
            // an account wondering why nothing is being scrobbled.
            settings->setEnableScrobbling(true);
            refresh();
        };
        handlers.failed = [refresh, token, status](const wxString& message) {
            refresh();
            if (!token.expired()) {
                status->setText(message);
            }
        };

        // CallAfter is documented as safe to call from a worker thread. Through
        // wxTheApp rather than through this dialog, for the reason given at
        // `paneAlive_` above: the reply must outlive the window.
        account_->connect(
            [](std::function<void()> action) { wxTheApp->CallAfter(std::move(action)); },
            std::move(handlers));
        refresh();
    });

    cancel->Bind(wxEVT_BUTTON, [this, refresh](wxCommandEvent&) {
        account_->cancelConnect();
        refresh();
    });

    forget->Bind(wxEVT_BUTTON, [this, refresh](wxCommandEvent&) {
        account_->forget();
        scrobbler_->setSession({});
        // Deliberately *not* switching the preference off. Disconnecting is
        // about the account, and a listener who reconnects should not also have
        // to remember to tick the box again. The queue is kept for the same
        // reason -- those plays really happened.
        refresh();
    });

    row->note(_("Plays are sent once you have heard half a track, or four "
                "minutes of it, whichever comes first. Tracks under 30 seconds "
                "are never scrobbled."));
    row->link(_("Your Last.fm applications"),
              "https://www.last.fm/settings/applications");
    row->note(_("Revoking access there stops scrobbling immediately, whatever "
                "this pane says."));

    refresh();

    return finishPane(pane, form);
}

}  // namespace xpcog::app
