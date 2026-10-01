// Preferences: the Sound section -- Output, Pitch & Tempo, MIDI. GTK's pages
// of the same names (app-gtk/src/PreferencesDialog.cpp), row for row.

#include "PreferencesRows.hpp"

#include "SpeedCurve.hpp"

#include "xpcog/core/audio/IAudioOutput.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::winui {

using app::Choice;
using app::tr;

void PreferencesWindow::buildOutputPage() {
    XPCOG_ROWS("output", tr("Output"), L"\xE7F5");

    std::vector<std::string> names;
    std::vector<std::string> ids;
    names.push_back(tr("System default"));
    ids.emplace_back();
    const std::string chosenId = settings_.rawValue("outputDeviceId");
    for (const DeviceInfo& device : enumerateOutputDevices()) {
        names.push_back(device.isDefault ? app::trf("%s (current default)", device.name) : device.name);
        ids.push_back(device.id);
    }
    uint32_t chosen = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == chosenId) {
            chosen = static_cast<uint32_t>(i);
            break;
        }
    }
    // A device chosen earlier and not plugged in now stays chosen, by name,
    // rather than silently becoming the default.
    if (chosen == 0 && !chosenId.empty()) {
        const std::string name = settings_.rawValue("outputDeviceName");
        names.push_back(app::trf("%s (not connected)", name.empty() ? chosenId : name));
        ids.push_back(chosenId);
        chosen = static_cast<uint32_t>(ids.size() - 1);
    }
    row->picker(tr("Output device"), names, chosen, [this, ids, names](uint32_t index) {
        if (index >= ids.size()) {
            return;
        }
        settings_.setRawValue("outputDeviceId", ids[index]);
        settings_.setRawValue("outputDeviceName", ids[index].empty() ? std::string{} : names[index]);
        settingChanged.publish("outputDeviceId");
    });
    row->toggle(tr("Play exclusively"), "exclusiveOutput",
                tr("Use the file's own rate and format instead of the system mixer's. Other "
                   "applications cannot play while this is active. Falls back to sharing if the "
                   "device is unavailable."));
    row->choice(tr("Volume scaling"), "volumeScaling", app::kVolumeScalingChoices);
    row->choice(tr("Resampler quality"), "resampling", app::kResamplingChoices);
    row->toggle(tr("Decode HDCD"), "enableHDCD",
                tr("Applies to 16-bit 44.1 kHz stereo only. Files without HDCD codes are "
                   "unaffected."));
    row->toggle(tr("Halve DSD volume"), "halveDSDVolume",
                tr("DSD is converted with a filter whose gain puts half modulation "
                   "\xE2\x80\x94 as loud as most SACDs go \xE2\x80\x94 at full "
                   "scale. Turn on if a loud SACD rip clips."));
    row->toggle(tr("Upmix stereo to surround"), "enableFSurround",
                tr("Uses FreeSurround. Takes effect when the device is next opened."));
    row->toggle(tr("Fade on seek and stop"), "enableFading");
    row->toggle(tr("Release the device while paused"), "suspendOutputOnPause",
                tr("Let other applications use the device while playback is paused. Turn off to "
                   "keep an exclusive device reserved."));
    row->note(tr("Volume scaling and resampler quality apply from the next track. Changing the "
                 "device moves playback across with a brief gap."));
}

void PreferencesWindow::buildPitchTempoPage() {
    XPCOG_ROWS("pitch-tempo", tr("Pitch & Tempo"), L"\xE916");

    struct Rows {
        Row pitch, tempo, lock, reset;
        Row transients, detector, phase, window, smoothing, formant, pitchMode, channels, note;
        mux::Controls::Slider    pitchScale{nullptr}, tempoScale{nullptr};
        mux::Controls::TextBlock pitchValue{nullptr}, tempoValue{nullptr};
        std::vector<std::string> windowValues;
        bool                     syncing = false;
    };
    auto rows = std::make_shared<Rows>();
    keep_.push_back(rows);

    // The window row is rebuilt for the engine: Finer has no long window.
    const auto rebuildWindow = [this, rows](bool finer) {
        std::string current = settings_.RubberbandWindow();
        if (finer && current == "long") {
            current = "standard";
            settings_.setRawValue("rubberbandWindow", current);
            settingChanged.publish("rubberbandWindow");
        }
        auto box = rows->window.control.as<mux::Controls::ComboBox>();
        rows->syncing = true;
        box.Items().Clear();
        rows->windowValues.clear();
        int32_t selected = 0;
        for (const Choice& option : app::kRubberWindowChoices) {
            if (finer && std::string_view{option.value} == "long") {
                continue;
            }
            if (current == option.value) {
                selected = static_cast<int32_t>(rows->windowValues.size());
            }
            box.Items().Append(winrt::box_value(toH(tr(option.label))));
            rows->windowValues.emplace_back(option.value);
        }
        box.SelectedIndex(selected);
        rows->syncing = false;
    };
    const auto refresh = [rows, rebuildWindow](const std::string& engine) {
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
    };

    row->choice(tr("Engine"), "rubberbandEngine", app::kStretchEngineChoices, refresh);

    const auto showValue = [](const mux::Controls::TextBlock& label, double ratio) {
        char buffer[24] = {};
        std::snprintf(buffer, sizeof(buffer), "%.2f\xC3\x97", ratio);
        label.Text(toH(buffer));
    };
    const auto speedRow = [&](const std::string& label, mux::Controls::Slider& scale,
                              mux::Controls::TextBlock& value, double ratio) {
        auto panel = mux::Controls::StackPanel();
        panel.Orientation(mux::Controls::Orientation::Horizontal);
        panel.Spacing(8);
        scale = mux::Controls::Slider();
        scale.Minimum(0);
        scale.Maximum(app::kSpeedSliderMax);
        scale.StepFrequency(1);
        scale.Width(220);
        // 1.00x is the one place on the curve worth finding by eye.
        scale.TickFrequency(app::sliderFromSpeed(1.0));
        scale.TickPlacement(mux::Controls::Primitives::TickPlacement::Outside);
        scale.IsThumbToolTipEnabled(false);
        scale.VerticalAlignment(mux::VerticalAlignment::Center);
        scale.Value(app::sliderFromSpeed(ratio));
        mux::Automation::AutomationProperties::SetName(scale, toH(label));
        value = secondaryText(L"VerticalAlignment='Center' MinWidth='52' TextAlignment='Right'");
        showValue(value, ratio);
        panel.Children().Append(scale);
        panel.Children().Append(value);
        return row->add(label, panel.as<mux::FrameworkElement>());
    };
    rows->pitch = speedRow(tr("Pitch"), rows->pitchScale, rows->pitchValue, settings_.Pitch());
    rows->tempo = speedRow(tr("Tempo"), rows->tempoScale, rows->tempoValue, settings_.Tempo());

    // One slider moved; with the lock on, the other follows it.
    const auto applySpeed = [this, rows, showValue](const char* key, const mux::Controls::Slider& scale,
                                                    const mux::Controls::TextBlock& value,
                                                    const char* otherKey,
                                                    const mux::Controls::Slider& otherScale,
                                                    const mux::Controls::TextBlock& otherValue) {
        if (rows->syncing) {
            return;
        }
        const double ratio =
            app::snapSpeed(app::speedFromSlider(static_cast<int>(std::lround(scale.Value()))));
        showValue(value, ratio);
        settings_.setRawValue(key, std::to_string(ratio));
        settingChanged.publish(key);
        if (settings_.SpeedLock()) {
            rows->syncing = true;
            otherScale.Value(app::sliderFromSpeed(ratio));
            rows->syncing = false;
            showValue(otherValue, ratio);
            settings_.setRawValue(otherKey, std::to_string(ratio));
            settingChanged.publish(otherKey);
        }
    };
    rows->pitchScale.ValueChanged([rows, applySpeed](auto&&, auto&&) {
        applySpeed("pitch", rows->pitchScale, rows->pitchValue, "tempo", rows->tempoScale,
                   rows->tempoValue);
    });
    rows->tempoScale.ValueChanged([rows, applySpeed](auto&&, auto&&) {
        applySpeed("tempo", rows->tempoScale, rows->tempoValue, "pitch", rows->pitchScale,
                   rows->pitchValue);
    });

    rows->lock = row->toggle(tr("Lock pitch and tempo together"), "speedLock",
                             tr("Moving either slider moves both, which is what a record "
                                "player's speed control does."));
    auto reset = mux::Controls::Button();
    reset.Content(winrt::box_value(toH(tr("Reset to 1.00\xC3\x97"))));
    reset.Click([this, rows, showValue](auto&&, auto&&) {
        rows->syncing = true;
        rows->pitchScale.Value(app::sliderFromSpeed(1.0));
        rows->tempoScale.Value(app::sliderFromSpeed(1.0));
        rows->syncing = false;
        showValue(rows->pitchValue, 1.0);
        showValue(rows->tempoValue, 1.0);
        settings_.setRawValue("pitch", "1");
        settings_.setRawValue("tempo", "1");
        settingChanged.publish("pitch");
        settingChanged.publish("tempo");
    });
    rows->reset = row->buttons({reset});

    rows->transients = row->choice(tr("Transients"), "rubberbandTransients", app::kRubberTransientsChoices);
    rows->detector   = row->choice(tr("Detector"), "rubberbandDetector", app::kRubberDetectorChoices);
    rows->phase      = row->choice(tr("Phase"), "rubberbandPhase", app::kRubberPhaseChoices);
    // Filled by rebuildWindow(), which knows which windows the engine has.
    rows->window = row->picker(tr("Window"), {}, 0, [this, rows](uint32_t selected) {
        if (rows->syncing) {
            return;
        }
        if (selected < rows->windowValues.size()) {
            settings_.setRawValue("rubberbandWindow", rows->windowValues[selected]);
            settingChanged.publish("rubberbandWindow");
        }
    });
    rows->smoothing = row->choice(tr("Smoothing"), "rubberbandSmoothing", app::kRubberSmoothingChoices);
    rows->formant   = row->choice(tr("Formant"), "rubberbandFormant", app::kRubberFormantChoices);
    rows->pitchMode = row->choice(tr("Pitch mode"), "rubberbandPitch", app::kRubberPitchChoices);
    rows->channels  = row->choice(tr("Channels"), "rubberbandChannels", app::kRubberChannelsChoices);
    rows->note      = row->note(tr("Varispeed resamples, as a record player would: one tempo "
                                   "slider, and the pitch follows it."));
    refresh(settings_.RubberbandEngine());
}

void PreferencesWindow::buildMidiPage() {
    XPCOG_ROWS("midi", "MIDI", L"\xEC4F");  // an acronym, the same in every language

    struct Rows {
        Row soundFont, roms, spessaNote, sc55Note;
    };
    auto rows = std::make_shared<Rows>();
    keep_.push_back(rows);
    const auto refresh = [rows](const std::string& synth) {
        const bool spessa = synth == "Spessa";
        const bool sc55   = synth == "NukeSc55";
        rows->soundFont.show(spessa);
        rows->spessaNote.show(spessa);
        rows->roms.show(sc55);
        rows->sc55Note.show(sc55);
    };
    row->choice(tr("Synthesiser"), "midiPlugin", app::kMidiSynthChoices, refresh);
    rows->soundFont = row->path(tr("SoundFont"), "soundFontPath", false, true,
                                {"*.sf2", "*.sf3", "*.sf2pack", "*.dls", "*.sflist", "*.json"});
    rows->roms = row->path(tr("SC-55 ROMs"), "midiRomPath", true, true, {"*.zip", "*.rar", "*.7z"});
    row->number(tr("Sample rate (Hz)"), "synthSampleRate", 8000, 192000, 100);
    row->number(tr("Default play time (s)"), "synthDefaultSeconds", 0.0, 3600.0, 0.1, 1);
    row->number(tr("Default fade time (s)"), "synthDefaultFadeSeconds", 0.0, 60.0, 0.1, 1);
    row->number(tr("Default loop count"), "synthDefaultLoopCount", 0, 10);
    rows->spessaNote = row->note(tr("SpessaSynth needs a bank: any .sf2, .sf3 or .dls. Files that "
                                    "carry their own bank use that instead."));
    rows->sc55Note   = row->note(tr("The SC-55 needs its five ROM files, which are not supplied. "
                                    "Choose the folder or the archive they came in; they are "
                                    "recognised by content, so nothing needs renaming. Without "
                                    "them, MIDI plays on the OPL3. It also ignores the sample rate "
                                    "and always renders at its own."));
    row->note(tr("These apply to every synthesised format, not only MIDI."));
    refresh(settings_.MidiPlugin());
}

}  // namespace xpcog::winui
