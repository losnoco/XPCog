// Every setting has to have a deliberate answer to "what happens when this
// changes", and this is what insists on it.
//
// The failure being guarded against is silent and has already happened. Most
// settings are read at the point of use, so writing one is the whole of the
// work. A minority are *held* -- in the engine's DSP chain, on the playlist, in
// a slider -- and for those a write that nothing acts on leaves the value stored
// and the player unchanged. `alwaysStopAfterCurrent` was exactly that until a
// branch was added for it: the box stayed ticked, the setting was stored, and
// playback carried on to the next track anyway.
//
// While a chain of ifs inside MainFrame held that knowledge, a setting added
// without a branch was a bug nobody could see. Now it is a test failure: the
// first case below walks Settings::all() and requires every key to be accounted
// for, either by doing something or by being named in the allow-list of keys
// that genuinely need nothing.

#include "SettingEffect.hpp"

#include "xpcog/core/Settings.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace xpcog;
using namespace xpcog::app;

namespace {

/// Keys that really do need nothing done when they change, because whatever
/// reads them reads them afresh: at the next track, the next scan, the next
/// launch, or the next time a notification is posted.
///
/// This list is the point of the test. Adding a key here is a claim -- "nothing
/// holds a copy of this" -- and it is meant to be made deliberately, in a review,
/// rather than by a setting quietly not matching any branch.
constexpr std::array kNothingToDo = {
    // Read when the engine opens a device or a track.
    "resampling", "halveDSDVolume", "outputDSDAsDoP", "suspendOutputOnPause",
    "volumeScaling",
    // Shown by the preferences pane when the device id no longer resolves;
    // nothing re-reads it.
    "outputDeviceName",
    // Written by the preset row into the eq keys, which carry the reload.
    "GraphicEQpreset",
    // Read while a folder is being walked.
    "readCueSheetsInFolders", "readPlaylistsInFolders", "skipAppleDoubleFiles",
    // Read when a synthesised track is opened.
    "synthSampleRate", "synthDefaultSeconds", "synthDefaultFadeSeconds",
    "synthDefaultLoopCount", "midiPlugin", "midiRomPath", "soundFontPath",
    // Read when the HTTP source opens a stream.
    "httpStreamingBufferSize",
    // Asked at the point the transport gets there.
    "keepPlayingWhileSkipping", "selectionFollowsPlayback",
    // Read once, during startup.
    "resumePlaybackOnStartup",
    // The catalogue is installed before the first window and wx cannot restate a
    // window's strings afterwards; widgetStyle is dead (see docs/WXPORT.md); and
    // closeToTray is answered by the platform when the window is closed.
    "language", "widgetStyle", "closeToTray",
    // Read when a notification is about to be posted.
    "notifications.enable", "notifications.show-album-art",
};

bool listed(std::string_view key) {
    return std::any_of(kNothingToDo.begin(), kNothingToDo.end(),
                       [key](const char* candidate) { return key == candidate; });
}

}  // namespace

TEST_CASE("every setting has a deliberate effect", "[settings][remote]") {
    std::vector<std::string> unaccounted;

    for (const Settings::Desc& desc : Settings::all()) {
        const SettingEffect effect = effectOf(desc.key);
        if (effect.effect == Effect::None && !listed(desc.key)) {
            unaccounted.emplace_back(desc.key);
        }
    }

    // Named in the failure rather than counted, because the useful thing to know
    // is which key was added without deciding this.
    INFO("settings with no effect and no entry in kNothingToDo: "
         << [&] {
                std::string joined;
                for (const std::string& key : unaccounted) {
                    joined += key;
                    joined += ' ';
                }
                return joined;
            }());
    CHECK(unaccounted.empty());
}

TEST_CASE("appliesFrom is one of the names the API promises", "[settings][remote]") {
    constexpr std::array kNames = {"immediately", "nextTrack", "nextDeviceOpen",
                                   "nextScan", "nextLaunch"};

    for (const Settings::Desc& desc : Settings::all()) {
        const std::string_view name = appliesFromName(effectOf(desc.key).applies);
        INFO("key: " << desc.key);
        CHECK(std::any_of(kNames.begin(), kNames.end(),
                          [name](const char* candidate) { return name == candidate; }));
    }
}

TEST_CASE("the keys that are held somewhere are the ones that act", "[settings][remote]") {
    // A spot check that the table says what the window needs it to say. These
    // are the four that were inert once, plus the two whose timing the API
    // reports differently.
    CHECK(effectOf("alwaysStopAfterCurrent").effect == Effect::PlaylistMode);
    CHECK(effectOf("repeat").effect == Effect::PlaylistMode);
    CHECK(effectOf("shuffle").effect == Effect::PlaylistMode);
    CHECK(effectOf("volume").effect == Effect::Volume);

    CHECK(effectOf("eq1kHz").effect == Effect::ReloadDsp);
    CHECK(effectOf("eq1kHz").applies == Applies::Immediately);

    // ReplayGain is read in AudioEngine::applyReplayGain() when a track opens,
    // and applyDspSettings() never looks at it -- so reloadDsp() does not reach
    // it and the honest answer is the next track. The chain this replaced routed
    // it through reloadDsp(), which was a no-op wearing the look of a fix.
    CHECK(effectOf("volumeScaling").effect == Effect::None);
    CHECK(effectOf("volumeScaling").applies == Applies::NextTrack);
}

TEST_CASE("session state is marked internal", "[settings][remote]") {
    // These are what the last session did, not preferences. The API reads them
    // and refuses to write them.
    for (const char* key : {"settingsSchemaVersion", "UserDefaultURLsKey",
                            "sentryAskedConsent", "lastPlaybackStatus", "miniMode",
                            "trayHideAnnounced", "outputDevice"}) {
        INFO("key: " << key);
        CHECK(effectOf(key).effect == Effect::Internal);
    }
}
