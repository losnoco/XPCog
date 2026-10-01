#include "SettingChoices.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace xpcog::app {
namespace {

/// Settings that already have a hand-written row in one of the panes above. The
/// generated pane skips them, so each setting is edited in exactly one place --
/// otherwise a curated list and a raw text box for the same key sit two clicks
/// apart, disagreeing about what the value should look like.
constexpr std::array kCuratedKeys = {
    // Playlist
    "alwaysStopAfterCurrent", "readCueSheetsInFolders", "readPlaylistsInFolders",
    "skipAppleDoubleFiles",
    "selectionFollowsPlayback", "resumePlaybackOnStartup",
    // Owned by a control outside this dialog, and listed here so that Advanced
    // does not offer a second one that disagrees with it. `volume` is the
    // transport slider; `repeat` and `shuffle` are the Order menu's radio
    // groups; `panelFollowMode` is View -> Panels Follow. All four are state a
    // gesture sets, not preferences someone comes here to type.
    "volume", "repeat", "shuffle", "panelFollowMode",
    // Output
    "volumeScaling", "resampling", "enableHDCD", "halveDSDVolume", "outputDeviceId",
    "outputDeviceName", "exclusiveOutput", "enableFSurround",
    "enableFading", "suspendOutputOnPause",
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
    // Both stay listed on macOS, where the pane has no row for either. Curated
    // is the right side of this list for them there too: closeToTray is
    // answered by the platform and widgetStyle is dead, and neither belongs in
    // Advanced, where a raw editable row would offer control that does not
    // exist.
    "widgetStyle", "closeToTray",
    // General. `language` has a picker there, and Advanced must not offer a
    // second one: its generated row would be a free-text box for a value that
    // has exactly three valid answers, one of which is the empty string.
    "language",
    // Visualizers
    "spectrumBarColor", "spectrumDotColor", "spectrumFreqMode", "spectrumFloorDb",
    "spectrumShowPeaks", "spectrumChannels", "scopeChannels", "scopeColor", "scopeBackgroundColor",
    "scopeStrokeWidth", "scopeGain", "scopeWindowMs", "scopeFrameRate", "scopeTrigger",
    "scopeFill", "scopeLogScale",
    // General
    "sentryConsented", "httpStreamingBufferSize",
    // Appearance
    "floatingMiniWindow", "waveformSeekBar", "waveformRectified", "waveformLogScale",
    "waveformHeight", "waveformPlayedColor", "waveformUnplayedColor",
    // Notifications
    "notifications.enable", "notifications.show-album-art",
    // Remote control. The token is not here at all -- it lives in the system
    // password store, not in settings.
    "remoteEnable", "remoteAddress", "remotePort", "remoteAllowWrite",
    // Scrobbling. Each switch sits on its service's pane beside the account it
    // means something for; the credentials are in the password store.
    "enableAudioScrobbler", "enableListenBrainz", "listenBrainzUrl",
    // Lyrics, on General; `lyricsSynced` is also View -> Timed Lyrics.
    "lyricsSynced", "enableLrclib", "lrclibUrl",
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

}  // namespace

bool hasCuratedRow(std::string_view key) {
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


bool isInternalKey(std::string_view key) { return contains(kInternalKeys, key); }


std::span<const Choice> choicesFor(std::string_view key) {
    // One place that knows which settings are pickers, so neither frontend has
    // to carry the list and they cannot disagree about it.
    if (key == "volumeScaling") {
        return kVolumeScalingChoices;
    }
    if (key == "resampling") {
        return kResamplingChoices;
    }
    if (key == "rubberbandEngine") {
        return kStretchEngineChoices;
    }
    if (key == "rubberbandTransients") {
        return kRubberTransientsChoices;
    }
    if (key == "rubberbandDetector") {
        return kRubberDetectorChoices;
    }
    if (key == "rubberbandPhase") {
        return kRubberPhaseChoices;
    }
    if (key == "rubberbandWindow") {
        return kRubberWindowChoices;
    }
    if (key == "rubberbandSmoothing") {
        return kRubberSmoothingChoices;
    }
    if (key == "rubberbandFormant") {
        return kRubberFormantChoices;
    }
    if (key == "rubberbandPitch") {
        return kRubberPitchChoices;
    }
    if (key == "rubberbandChannels") {
        return kRubberChannelsChoices;
    }
    if (key == "midiPlugin") {
        return kMidiSynthChoices;
    }
    return {};
}

std::string choiceLabel(std::span<const Choice> choices, std::string_view value) {
    for (const Choice& choice : choices) {
        if (value == choice.value) {
            return tr(choice.label);
        }
    }
    // Not in the list, which is a settings file from a build that offered more.
    // Shown as the stored value rather than as the first row: the picker is
    // reporting what is set, and pretending otherwise would make choosing
    // anything else silently discard it.
    return std::string(value);
}

}  // namespace xpcog::app
