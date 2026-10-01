#include "PreferencesRows.hpp"

// The first group of pages -- General, Playlist, Appearance, Notifications,
// Visualizers -- as app-gtk/src/PreferencesDialog.cpp has them: the same rows,
// keys, conditions and wording. Where the two players differ it is said here.

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/platform/CrashReporter.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <utility>

namespace xpcog::winui {

using app::Choice;
using app::tr;

namespace {

/// "#rrggbb", lower case: the spelling the settings hold.
[[nodiscard]] std::string hexOf(winrt::Windows::UI::Color colour) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", colour.R, colour.G, colour.B);
    return buffer;
}

}  // namespace

void PreferencesWindow::buildGeneralPage() {
    XPCOG_ROWS("general", tr("General"), L"\xE713");

    std::vector<std::string> languageNames;
    std::vector<std::string> languageCodes;
    uint32_t                 chosen = 0;
    for (const app::LanguageOption& option : app::availableLanguages()) {
        if (option.code == settings_.Language()) {
            chosen = static_cast<uint32_t>(languageCodes.size());
        }
        languageNames.push_back(option.code.empty() ? tr("Follow the system") : option.name);
        languageCodes.push_back(option.code);
    }
    row->picker(tr("Language"), languageNames, chosen, [this, languageCodes](uint32_t index) {
        if (index >= languageCodes.size()) {
            return;
        }
        settings_.setLanguage(languageCodes[index]);
        settingChanged.publish("language");
        settings_.sync();
    });
    row->note(tr("Restart XPCog to apply."));

    row->number(tr("Streaming buffer (bytes)"), "httpStreamingBufferSize", 65536, 134217728, 65536);
    row->note(tr("How much of an internet radio stream is read ahead. Raise it for a slow or "
                 "distant station."));

    const Row consent = row->toggle(tr("Send crash reports and usage data"), "sentryConsented");
    if (platform::crashReportingAvailable()) {
        row->note(tr("Nothing is collected or sent while this is off."));
        row->link(tr("Privacy policy"), platform::kPrivacyPolicyUrl);
    } else {
        consent.enable(false);
        row->note(tr("Crash reporting is not included in this build."));
    }

    row->heading(tr("Lyrics"));
    // How, before where from: it applies to the file's own lyrics as much as
    // to LRCLIB's, so it does not belong under the switch that sends
    // something out.
    row->toggle(tr("Follow timed lyrics line by line"), "lyricsSynced");
    row->note(tr("Off, timed lyrics are shown as plain text, and a file's own "
                 "lyrics tag is preferred over a .lrc file beside it."));
    const Row lyrics = row->toggle(tr("Look up lyrics on LRCLIB when the file has none"), "enableLrclib");
    if (httpClientAvailable()) {
        row->note(tr("The artist, title, album and length of the track you are looking at are "
                     "sent to LRCLIB, a free lyrics service with no accounts. Each answer is kept "
                     "in the library, so a track is asked about once."));
        row->link(tr("About LRCLIB"), "https://lrclib.net/");
        row->text(tr("API address"), "lrclibUrl");
    } else {
        lyrics.enable(false);
        row->note(tr("This build was configured without HTTP support, so it cannot reach LRCLIB."));
    }
}

void PreferencesWindow::buildPlaylistPage() {
    XPCOG_ROWS("playlist", tr("Playlist"), L"\xE8FD");
    row->toggle(tr("Stop after every track"), "alwaysStopAfterCurrent");
    row->toggle(tr("Follow the playing track in the playlist"), "selectionFollowsPlayback",
                tr("Move the selection to each track as it starts."));
    row->toggle(tr("Keep playing while looking for the next playable track"),
                "keepPlayingWhileSkipping",
                tr("When Next or Previous lands on a track that will not open, carry on playing "
                   "the current one until a playable track is found."));
    row->toggle(tr("Resume playback on startup"), "resumePlaybackOnStartup",
                tr("Continue the last track from where it stopped. The track is selected either "
                   "way."));
    row->toggle(tr("Read cue sheets when adding folders"), "readCueSheetsInFolders");
    row->toggle(tr("Read playlists when adding folders"), "readPlaylistsInFolders");
    row->toggle(tr("Ignore AppleDouble files"), "skipAppleDoubleFiles",
                tr("The \"._\" files macOS leaves beside the audio. They keep the track's "
                   "extension but hold no audio."));
}

void PreferencesWindow::buildAppearancePage() {
    XPCOG_ROWS("appearance", tr("Appearance"), L"\xE790");

    const Row closeToTray = row->toggle(tr("Close to tray"), "closeToTray",
                                        tr("Closing the window leaves XPCog running in the "
                                           "notification area instead of quitting."));
    // Offered only where there is somewhere to hide to, as the wx and GTK
    // dialogs do: a switch that does nothing is worse than an absent one. The
    // WinUI player has no tray icon yet, so for now this is always the case.
    if (!hasTray_) {
        closeToTray.enable(false);
        row->note(tr("This session has no notification area to keep XPCog in."));
    }
    // Live here, unlike in the GTK dialog: a Windows window can be kept above
    // the others (the wx mini player does it), where GTK 4 and Wayland cannot.
    row->toggle(tr("Keep the mini player on top"), "floatingMiniWindow");

    const Row show = row->toggle(tr("Show the waveform in the seek bar"), "waveformSeekBar",
                                 tr("The track's shape behind the playhead, analysed the first "
                                    "time it plays and kept for every play after."));
    const Row rectified = row->toggle(tr("Rectified: stand the waveform on the bottom edge"),
                                      "waveformRectified",
                                      tr("Instead of mirroring it about the centre line."));
    const Row logarithmic = row->toggle(tr("Logarithmic: draw levels in decibels"), "waveformLogScale",
                                        tr("So quiet material is a shape rather than a line."));
    const Row height = row->number(tr("Height"), "waveformHeight", 20, 80);

    // A colour that can instead follow the theme: a switch saying it follows,
    // and the picker under it, live only while the switch is off. Following
    // empties the key and the theme answers; not following writes the
    // picker's colour, which is the fallback until it is moved.
    const auto colourWithDefault = [&](const std::string& follow, const std::string& pick,
                                       const char* key, const char* fallback) {
        const bool following = settings_.rawValue(key).empty();
        auto       follows   = mux::Controls::ToggleSwitch();
        follows.MinWidth(0);
        follows.IsOn(following);
        const Row followRow = row->add(follow, follows);
        const Row pickerRow = row->colour(pick, key, fallback);
        if (following) {
            // row->colour() writes nothing until it is moved, so the key stays
            // empty; but the row must say it is not in charge.
            pickerRow.enable(false);
        }
        follows.Toggled([this, key, follows, pickerRow](auto&&, auto&&) {
            const bool on = follows.IsOn();
            pickerRow.enable(!on);
            const auto picker = pickerRow.control.as<mux::Controls::Button>()
                                    .Flyout()
                                    .as<mux::Controls::Flyout>()
                                    .Content()
                                    .as<mux::Controls::ColorPicker>();
            settings_.setRawValue(key, on ? std::string{} : hexOf(picker.Color()));
            settingChanged.publish(key);
        });
        return std::pair{followRow, pickerRow};
    };
    const auto played   = colourWithDefault(tr("Played part follows the system accent colour"),
                                            tr("Played colour"), "waveformPlayedColor", "#0a84ff");
    const auto unplayed = colourWithDefault(tr("Unplayed part follows the theme's text colour"),
                                            tr("Unplayed colour"), "waveformUnplayedColor", "#808080");

    // The styles only mean something while the waveform is shown.
    const auto styles = [=](bool on) {
        rectified.enable(on);
        logarithmic.enable(on);
        height.enable(on);
        for (const auto& pair : {played, unplayed}) {
            pair.first.enable(on);
            const bool following = pair.first.control.as<mux::Controls::ToggleSwitch>().IsOn();
            pair.second.enable(on && !following);
        }
    };
    styles(settings_.WaveformSeekBar());
    show.control.as<mux::Controls::ToggleSwitch>().Toggled([styles](auto const& sender, auto&&) {
        styles(sender.template as<mux::Controls::ToggleSwitch>().IsOn());
    });
}

void PreferencesWindow::buildNotificationsPage() {
    XPCOG_ROWS("notifications", tr("Notifications"), L"\xEA8F");
    row->toggle(tr("Enable notifications"), "notifications.enable");
    row->toggle(tr("Show album art"), "notifications.show-album-art");
    row->note(tr("Shown as each track starts. Focus Assist or Do Not Disturb can hold "
                 "notifications back."));
}

void PreferencesWindow::buildVisualizersPage() {
    XPCOG_ROWS("visualizers", tr("Visualizers"), L"\xE9D9");

    row->heading(tr("Spectrum"));
    row->picker(tr("Bands"),
                {tr("Musical notes (one bar per semitone)"), tr("Even frequency spacing")},
                settings_.SpectrumFreqMode() ? 1 : 0, [this](uint32_t index) {
                    settings_.setSpectrumFreqMode(index == 1);
                    settingChanged.publish("spectrumFreqMode");
                });
    static constexpr std::array kSpectrumChannels = {
        Choice{"mono", XPCOG_TRANSLATE("Mono")},
        Choice{"left", XPCOG_TRANSLATE("Left")},
        Choice{"right", XPCOG_TRANSLATE("Right")},
        Choice{"mirrored", XPCOG_TRANSLATE("Stereo, mirrored")},
        Choice{"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
        Choice{"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
    };
    row->choice(tr("Channels"), "spectrumChannels", kSpectrumChannels);
    row->colour(tr("Bar colour"), "spectrumBarColor", "#ff8000");
    row->colour(tr("Peak colour"), "spectrumDotColor", "#ff3b30");
    row->toggle(tr("Show peak markers"), "spectrumShowPeaks");
    row->number(tr("Quietest level shown (dB)"), "spectrumFloorDb", -120, -20);
    row->note(tr("Bars sit on semitones from C0, so the display lines up with the notes being "
                 "played."));

    row->heading(tr("Oscilloscope"));
    static constexpr std::array kChannels = {
        Choice{"mono", XPCOG_TRANSLATE("Mono")},
        Choice{"left", XPCOG_TRANSLATE("Left")},
        Choice{"right", XPCOG_TRANSLATE("Right")},
        Choice{"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
        Choice{"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
    };
    row->choice(tr("Channels"), "scopeChannels", kChannels);
    row->colour(tr("Trace colour"), "scopeColor", "#30d158");
    row->colour(tr("Background"), "scopeBackgroundColor", "#121214");
    row->number(tr("Stroke width"), "scopeStrokeWidth", 0.5, 6.0, 0.5, 1);
    row->number(tr("Vertical gain"), "scopeGain", 0.25, 8.0, 0.25, 2);
    row->number(tr("Window (ms)"), "scopeWindowMs", 5, 100);
    row->number(tr("Frames per second"), "scopeFrameRate", 15, 120);
    row->toggle(tr("Hold a steady tone still"), "scopeTrigger",
                tr("Start each frame at a rising zero crossing, so a tone does not crawl across "
                   "the display."));
    row->toggle(tr("Fill under the trace"), "scopeFill");
    row->toggle(tr("Logarithmic scale"), "scopeLogScale",
                tr("Levels in decibels rather than linear, so quiet material is a shape rather "
                   "than a line."));
}

}  // namespace xpcog::winui
