#include "PreferencesDialog.hpp"

#include "Text.hpp"

#include "xpcog/core/audio/IAudioOutput.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/taskbar.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::app {
namespace {

/// A setting whose values are a closed set, so it deserves a named list rather
/// than a text box. The strings are Cog's stored values, unchanged.
struct Choice {
    const char* value;
    const char* label;
};

constexpr std::array kVolumeScalingChoices = {
    Choice{"none", "None"},
    Choice{"volumeScale", "Volume tag"},
    Choice{"soundcheck", "iTunes Sound Check"},
    Choice{"trackGain", "Track gain"},
    Choice{"trackGainWithPeak", "Track gain, peak-limited"},
    Choice{"albumGain", "Album gain"},
    Choice{"albumGainWithPeak", "Album gain, peak-limited"},
};

constexpr std::array kResamplingChoices = {
    Choice{"quick", "Quick"}, Choice{"low", "Low"},   Choice{"medium", "Medium"},
    Choice{"high", "High"},   Choice{"best", "Best"},
};

/// The synthesisers `midiPlugin` can name, in Cog's own spelling.
///
/// Nuked OPL3 twice over -- id's DMX driver, once per instrument bank, and
/// Nuke.YKT's General MIDI one -- and then an emulated Roland. The OPL labels are
/// the drivers' own bank names (vendor/nuked-opl3), spelled out here rather than
/// read back from them so the dialog does not have to construct a synthesiser to
/// draw a list. See docs/MIDI.md.
constexpr std::array kMidiSynthChoices = {
    Choice{"DOOM0", "OPL3 \xE2\x80\x94 DMX default"},
    Choice{"DOOM1", "OPL3 \xE2\x80\x94 DMX Doom"},
    Choice{"DOOM2", "OPL3 \xE2\x80\x94 DMX Doom II"},
    Choice{"DOOM3", "OPL3 \xE2\x80\x94 DMX Raptor"},
    Choice{"DOOM4", "OPL3 \xE2\x80\x94 DMX Strife"},
    Choice{"DOOM5", "OPL3 \xE2\x80\x94 DMXOPL"},
    Choice{"OPL3W0", "OPL3 \xE2\x80\x94 General MIDI"},
    Choice{"Spessa", "SoundFont \xE2\x80\x94 SpessaSynth"},
    Choice{"NukeSc55", "Roland SC-55"},
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
    "alwaysStopAfterCurrent", "readCueSheetsInFolders",
    // Output
    "volumeScaling", "resampling", "enableHDCD", "halveDSDVolume", "outputDeviceId",
    "outputDeviceName", "exclusiveOutput", "enableFSurround", "enableFading",
    "suspendOutputOnPause",
    // MIDI
    "midiPlugin", "midiRomPath", "soundFontPath", "synthSampleRate",
    "synthDefaultSeconds", "synthDefaultFadeSeconds", "synthDefaultLoopCount",
    // Appearance
    //
    // floatingMiniWindow is deliberately absent: the mini player carries its own
    // control for it and applies it as it is clicked.
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
    // Spectrum
    "spectrumBarColor", "spectrumDotColor", "spectrumFreqMode", "spectrumFloorDb",
    "spectrumShowPeaks",
};

/// Not settings at all, but internal state that happens to live in the same
/// store. Shown, because the generated pane's whole point is that nothing is
/// hidden, but not editable: settingsSchemaVersion drives
/// Settings::applyMigrations(), so typing into it makes migrations re-run or be
/// skipped, and nothing about a spin box suggests that. UserDefaultURLsKey is the
/// Open URL history -- a newline-separated list the dialog maintains, where a
/// hand edit can only produce entries that will not parse.
constexpr std::array kInternalKeys = {"settingsSchemaVersion", "UserDefaultURLsKey"};

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
    return contains(kCuratedKeys, key);
}

/// A two-column form: a caption and a control, with the control column growing.
/// wxFlexGridSizer is what QFormLayout was.
[[nodiscard]] wxFlexGridSizer* makeForm(int gap) {
    auto* form = new wxFlexGridSizer(2, gap, gap * 2);
    form->AddGrowableCol(1, 1);
    return form;
}

/// Adds curated rows to one pane's form.
///
/// A struct rather than a lambda per pane because there are four panes wanting
/// the same handful of row kinds, and four copies of "read the raw value, write
/// it back, announce it" is four chances for one of them to forget the
/// announcement. It reports changes through a callback rather than publishing the
/// dialog's own signal, which is not something a helper should be reaching into.
class RowBuilder : public wxClientData {
public:
    using Announce = std::function<void(const char*)>;

    RowBuilder(Settings& settings, wxWindow* pane, wxFlexGridSizer* form,
               Announce announce)
        : settings_(&settings), pane_(pane), form_(form), announce_(std::move(announce)) {}

    void choice(const char* label, const char* key,
                std::span<const Choice> choices) const {
        wxArrayString items;
        for (const Choice& option : choices) {
            items.Add(wxString::FromUTF8(option.label));
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
                                 values](wxCommandEvent& event) {
            const auto selected = static_cast<std::size_t>(event.GetSelection());
            if (selected < values.size()) {
                settings->setRawValue(key, values[selected]);
                announce(key);
            }
        });
        add(label, box);
    }

    void toggle(const char* label, const char* key, const char* hint = nullptr) const {
        auto* box = new wxCheckBox(pane_, wxID_ANY, wxString::FromUTF8(label));
        box->SetValue(isTrue(settings_->rawValue(key)));
        if (hint != nullptr) {
            box->SetToolTip(wxString::FromUTF8(hint));
        }
        box->Bind(wxEVT_CHECKBOX,
                  [settings = settings_, announce = announce_, key](wxCommandEvent& event) {
                      settings->setRawValue(key, event.IsChecked() ? "true" : "false");
                      announce(key);
                  });
        add("", box);
    }

    void number(const char* label, const char* key, int minimum, int maximum) const {
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

    void seconds(const char* label, const char* key, double maximum) const {
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
    void file(const char* label, const char* key, const char* filter) const {
        auto* edit = makeEdit(key);

        auto* browse = new wxButton(pane_, wxID_ANY, "Choose...");
        browse->Bind(wxEVT_BUTTON, [this, edit, key, label, filter](wxCommandEvent&) {
            apply(edit, key,
                  wxFileSelector(wxString::FromUTF8(label), wxEmptyString,
                                 wxEmptyString, wxEmptyString,
                                 wxString::FromUTF8(filter),
                                 wxFD_OPEN | wxFD_FILE_MUST_EXIST, pane_));
        });

        addWithButtons(label, edit, {browse});
    }

    /// A path the listener has to supply, with the two ways of choosing one.
    ///
    /// Two buttons rather than one because the thing being named may be either a
    /// folder or a file, and no file dialog on any platform offers both at once.
    /// The text box accepts a typed or pasted path either way.
    void path(const char* label, const char* key, const char* archiveFilter) const {
        auto* edit = makeEdit(key);

        auto* folder = new wxButton(pane_, wxID_ANY, "Folder...");
        folder->Bind(wxEVT_BUTTON, [this, edit, key, label](wxCommandEvent&) {
            apply(edit, key,
                  wxDirSelector(wxString::FromUTF8(label), wxEmptyString,
                                wxDD_DEFAULT_STYLE, wxDefaultPosition, pane_));
        });

        auto* archive = new wxButton(pane_, wxID_ANY, "Archive...");
        archive->Bind(wxEVT_BUTTON,
                      [this, edit, key, label, archiveFilter](wxCommandEvent&) {
                          apply(edit, key,
                                wxFileSelector(wxString::FromUTF8(label), wxEmptyString,
                                               wxEmptyString, wxEmptyString,
                                               wxString::FromUTF8(archiveFilter),
                                               wxFD_OPEN | wxFD_FILE_MUST_EXIST,
                                               pane_));
                      });

        addWithButtons(label, edit, {folder, archive});
    }

    void note(const char* text) const {
        auto* label = new wxStaticText(pane_, wxID_ANY, wxString::FromUTF8(text));
        label->Wrap(pane_->FromDIP(440));
        label->Enable(false);
        form_->AddSpacer(0);
        form_->Add(label, 1, wxEXPAND | wxTOP, pane_->FromDIP(6));
    }

    void add(const char* label, wxWindow* control) const {
        form_->Add(new wxStaticText(pane_, wxID_ANY, wxString::FromUTF8(label)), 0,
                   wxALIGN_CENTER_VERTICAL);
        form_->Add(control, 1, wxEXPAND);
    }

private:
    /// A text box that writes its setting when it loses focus or takes Return.
    [[nodiscard]] wxTextCtrl* makeEdit(const char* key) const {
        auto* edit = new wxTextCtrl(pane_, wxID_ANY, toWx(settings_->rawValue(key)),
                                    wxDefaultPosition, wxDefaultSize,
                                    wxTE_PROCESS_ENTER);
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

    void addWithButtons(const char* label, wxTextCtrl* edit,
                        std::initializer_list<wxButton*> buttons) const {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(edit, 1, wxALIGN_CENTER_VERTICAL);
        for (wxButton* button : buttons) {
            row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, pane_->FromDIP(4));
        }
        form_->Add(new wxStaticText(pane_, wxID_ANY, wxString::FromUTF8(label)), 0,
                   wxALIGN_CENTER_VERTICAL);
        form_->Add(row, 1, wxEXPAND);
    }

    // Held by pointer, and every handler captures the pointer rather than `this`:
    // a RowBuilder is a local in the function that builds a pane and is gone by
    // the time anyone clicks anything, while the settings object and the callback
    // both outlive the dialog.
    Settings*        settings_;
    wxWindow*        pane_;
    wxFlexGridSizer* form_;
    Announce         announce_;
};

}  // namespace

PreferencesDialog::PreferencesDialog(wxWindow* parent, Settings& settings)
    : wxDialog(parent, wxID_ANY, "Preferences", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      settings_(settings) {
    SetSize(FromDIP(wxSize(660, 480)));

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
    page(buildPlaylistPane(book), "Playlist");
    page(buildOutputPane(book), "Output");
    // Absent on macOS. Its only control is the close-to-tray checkbox, which is
    // already Windows and Linux only -- macOS closes to the Dock unconditionally,
    // by platform convention rather than by preference -- so what remains there is
    // a category whose page is one greyed-out paragraph. That reads as a screen
    // that failed to load, and the paragraph says nothing a macOS user needs
    // telling: following the system appearance is what every application on the
    // platform does.
#ifndef __WXOSX__
    page(buildAppearancePane(book), "Appearance");
#endif
    page(buildMidiPane(book), "MIDI");
    page(buildSpectrumPane(book), "Spectrum");
    page(buildAdvancedPane(book), "Advanced");

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

std::function<void(const char*)> PreferencesDialog::changeNotifier() {
    return [this](const char* key) { settingChanged.publish(key); };
}

wxWindow* PreferencesDialog::buildPlaylistPane(wxWindow* parent) {
    auto* pane = new wxPanel(parent, wxID_ANY);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    row->toggle("Stop after every track", "alwaysStopAfterCurrent");
    row->toggle("Read cue sheets when adding folders", "readCueSheetsInFolders");

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    return pane;
}

wxWindow* PreferencesDialog::buildOutputPane(wxWindow* parent) {
    auto* pane = new wxPanel(parent, wxID_ANY);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // The device list is read once, when this pane is built. Enumerating spins up
    // the backend's context, and a picker that re-enumerated on every repaint
    // would do that while the listener is dragging a slider next to it.
    wxArrayString            names;
    std::vector<std::string> ids;
    names.Add("System default");
    ids.emplace_back();

    const std::string chosenId = settings_.rawValue("outputDeviceId");
    for (const DeviceInfo& device : enumerateOutputDevices()) {
        // Named rather than left to be guessed at: "System default" and the
        // device it currently resolves to are different rows, and which one is
        // chosen matters when headphones are plugged in later.
        names.Add(toWx(device.isDefault ? device.name + " (current default)"
                                        : device.name));
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
        names.Add(toWx((name.empty() ? chosenId : name) + " (not connected)"));
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
    row->add("Output device", deviceBox);

    row->toggle("Take the device exclusively", "exclusiveOutput",
                "Plays at the file's own rate and format instead of the system "
                "mixer's, and silences everything else on the machine while it does. "
                "Falls back to sharing when the device or the platform will not "
                "allow it.");

    row->choice("Volume scaling", "volumeScaling", kVolumeScalingChoices);
    row->choice("Resampler quality", "resampling", kResamplingChoices);

    row->toggle("Decode HDCD", "enableHDCD",
                "Only affects 16-bit 44.1 kHz stereo lossless material, and is "
                "bit-transparent on files carrying no HDCD codes.");
    row->toggle("Halve the volume of DSD", "halveDSDVolume",
                "DSD is converted with a filter whose gain puts half modulation "
                "\xE2\x80\x94 as loud as most SACDs go \xE2\x80\x94 at full scale. "
                "Turn this on if a recording that goes louder is clipping.");
    row->toggle("FreeSurround stereo-to-surround upmix", "enableFSurround");
    row->toggle("Fade on seek and stop", "enableFading");
    row->toggle("Release the audio device while paused", "suspendOutputOnPause");

    row->note("Volume scaling and resampler quality take effect on the next track. "
              "The device and exclusive mode move whatever is playing on to the new "
              "device, with a short break while it opens.");

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    return pane;
}

wxWindow* PreferencesDialog::buildMidiPane(wxWindow* parent) {
    auto* pane = new wxPanel(parent, wxID_ANY);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    row->choice("Synthesiser", "midiPlugin", kMidiSynthChoices);
    row->file("SoundFont", "soundFontPath",
              "SoundFont banks|*.sf2;*.sf3;*.sf2pack;*.dls;*.sflist;*.json|All files|*.*");
    row->path("SC-55 ROMs", "midiRomPath", "Archives|*.zip;*.rar;*.7z|All files|*.*");

    // Cog's clamps, and its labels. The sample rate is the rate a synthesiser
    // renders at rather than the rate the file plays at -- there is no such thing
    // as the second one for a score.
    row->number("Sample rate (Hz)", "synthSampleRate", 8000, 192000);
    row->seconds("Default play time (s)", "synthDefaultSeconds", 3600.0);
    row->seconds("Default fade time (s)", "synthDefaultFadeSeconds", 60.0);
    row->number("Default loop count", "synthDefaultLoopCount", 0, 10);

    row->note("SpessaSynth plays a SoundFont bank, and has no sound of its own "
              "without one \xE2\x80\x94 any .sf2, .sf3 or .dls will do. A file that "
              "has a bank of its own beside it is played with that one instead, "
              "whatever is chosen here.");
    row->note("The Roland needs its five ROM files, which are not something this "
              "player can supply. Name either the folder holding them or the archive "
              "they came in \xE2\x80\x94 they are recognised by content, so nothing "
              "has to be renamed. Without them a MIDI file still plays, on the OPL3.");
    row->note("The sample rate and the defaults below it apply to every synthesised "
              "format, not only MIDI \xE2\x80\x94 a game music rip has no length of "
              "its own either. The Roland ignores the sample rate: it renders at the "
              "rate its hardware ran at and nothing else.");

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    return pane;
}

// Not built on macOS at all -- see the page list in the constructor. The guard is
// here rather than only around the caller so the pane's one control keeps its
// single `#ifndef`, instead of an empty function surviving for no one to call.
#ifndef __WXOSX__
wxWindow* PreferencesDialog::buildAppearancePane(wxWindow* parent) {
    auto* pane = new wxPanel(parent, wxID_ANY);
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
    auto* closeToTray =
        new wxCheckBox(pane, wxID_ANY, "Closing the window keeps XPCog running");
    closeToTray->SetValue(settings_.CloseToTray());
    // Offered only where there is somewhere to hide to. Without a notification
    // area this would hide the window with no way to bring it back, so the
    // setting is ignored at the call site *and* disabled here -- a checkbox that
    // does nothing is worse than an absent one, and this at least says why.
    if (!wxTaskBarIcon::IsAvailable()) {
        closeToTray->Enable(false);
        closeToTray->SetToolTip("This session has no notification area to keep "
                                "XPCog in.");
    }
    closeToTray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        settings_.setCloseToTray(event.IsChecked());
        settingChanged.publish("closeToTray");
    });
    row->add("", closeToTray);

    auto* note = new wxStaticText(
        pane, wxID_ANY,
        "XPCog draws with the platform's own controls, so it follows the system "
        "appearance rather than offering a theme of its own. Switching the system "
        "to dark mode re-strokes the interface icons while the window is open.");
    note->Wrap(pane->FromDIP(440));
    note->Enable(false);
    form->AddSpacer(0);
    form->Add(note, 1, wxEXPAND | wxTOP, pane->FromDIP(6));

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    return pane;
}
#endif  // !__WXOSX__

wxWindow* PreferencesDialog::buildSpectrumPane(wxWindow* parent) {
    auto* pane = new wxPanel(parent, wxID_ANY);
    auto* form = makeForm(pane->FromDIP(6));
    auto* row  = new RowBuilder{settings_, pane, form, changeNotifier()};
    pane->SetClientObject(row);

    // Bands. Cog's two analyser modes, stored in its own key: false is the note
    // scale, true the even spacing. A list rather than a checkbox because
    // "Frequency mode: off" says nothing about what you get instead.
    wxArrayString bandModes;
    bandModes.Add("Musical notes (one bar per semitone)");
    bandModes.Add("Even frequency spacing");
    auto* bands = new wxChoice(pane, wxID_ANY, wxDefaultPosition, wxDefaultSize, bandModes);
    bands->SetSelection(settings_.SpectrumFreqMode() ? 1 : 0);
    bands->Bind(wxEVT_CHOICE, [this](wxCommandEvent& event) {
        settings_.setSpectrumFreqMode(event.GetSelection() == 1);
        settingChanged.publish("spectrumFreqMode");
    });
    row->add("Bands", bands);

    // wxColourPickerCtrl is a real colour button -- what the Qt version built
    // from a QPushButton, a generated swatch pixmap and QColorDialog.
    //
    // An unparseable stored value -- notably an imported Cog colour, which is an
    // archived NSColor rather than a string -- shows as the setting's own default
    // rather than as black. A black bar on a near-black background looks like the
    // display is broken.
    const auto colourRow = [&](const char* label, const std::string& stored,
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

    colourRow("Bar colour", settings_.SpectrumBarColor(), [this](const std::string& hex) {
        settings_.setSpectrumBarColor(hex);
        settingChanged.publish("spectrumBarColor");
    });
    colourRow("Peak colour", settings_.SpectrumDotColor(), [this](const std::string& hex) {
        settings_.setSpectrumDotColor(hex);
        settingChanged.publish("spectrumDotColor");
    });

    auto* peaks = new wxCheckBox(pane, wxID_ANY, "Show peak markers");
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
    row->add("Quietest level shown (dB)", floorDb);

    row->note("Bars sit on semitones from C0, so a spectrum of music lines up with "
              "the notes being played. Below a few hundred hertz several bars share "
              "one analysis bin and move together \xE2\x80\x94 that is the resolution "
              "of the window, not a fault.");

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    return pane;
}

wxWindow* PreferencesDialog::buildAdvancedPane(wxWindow* parent) {
    // Scrolled, because the list grows with every milestone and a fixed pane
    // would quietly start clipping.
    auto* pane = new wxScrolled<wxPanel>(parent, wxID_ANY);
    pane->SetScrollRate(0, pane->FromDIP(8));

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
            editor->SetToolTip("Maintained automatically, and not meant to be edited.");
        }
        row->add(label.c_str(), editor);
    }

    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(form, 1, wxEXPAND | wxALL, pane->FromDIP(10));
    pane->SetSizer(layout);
    pane->FitInside();
    return pane;
}

}  // namespace xpcog::app
