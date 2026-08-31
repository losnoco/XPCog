#include "SettingEffect.hpp"

namespace xpcog::app {

std::string_view appliesFromName(Applies applies) {
    switch (applies) {
        case Applies::Immediately:    return "immediately";
        case Applies::NextTrack:      return "nextTrack";
        case Applies::NextDeviceOpen: return "nextDeviceOpen";
        case Applies::NextScan:       return "nextScan";
        case Applies::NextLaunch:     return "nextLaunch";
    }
    return "immediately";
}

SettingEffect effectOf(std::string_view key) {
    // The equaliser is 33 keys -- eqPreamp, the 31 bands and the enable -- and
    // they are matched by prefix for the same reason PreferencesDialog matches
    // them by prefix: listing them is 33 lines that say one thing.
    if (key.starts_with("eq") || key == "GraphicEQenable") {
        return {Effect::ReloadDsp, Applies::Immediately};
    }

    // The stretch options the DSP thread reads on the next pass. Both the popup
    // on the strip and the preferences pane can move these, so the other one is
    // showing a stale number until it is told -- RefreshSpeed is that telling.
    if (key == "pitch" || key == "tempo" || key == "speedLock" ||
        key == "rubberbandEngine") {
        return {Effect::RefreshSpeed, Applies::Immediately};
    }
    if (key.starts_with("rubberband")) {
        return {Effect::ReloadDsp, Applies::Immediately};
    }

    if (key.starts_with("spectrum")) {
        return {Effect::RefreshSpectrum, Applies::Immediately};
    }

    if (key == "enableFading" || key == "enableFSurround" || key == "enableHDCD") {
        return {Effect::ReloadDsp, Applies::Immediately};
    }

    // Read in applyReplayGain() when a track opens, which reloadDsp() does not
    // reach -- applyDspSettings() never looks at it. So there is nothing to do
    // and nothing to claim: it moves on the next track by itself. The chain this
    // replaced routed it through reloadDsp(), which was a no-op wearing the look
    // of a fix.
    if (key == "volumeScaling") {
        return {Effect::None, Applies::NextTrack};
    }

    if (key == "outputDeviceId" || key == "exclusiveOutput") {
        return {Effect::ReopenOutput, Applies::Immediately};
    }
    // Named for the reader's benefit rather than acted on: the device is chosen
    // by id, and the name is what the preferences pane shows when that id is
    // gone. Nothing re-reads it.
    if (key == "outputDeviceName") {
        return {Effect::None, Applies::Immediately};
    }
    // Read when the engine opens a device or a track, so a change lands with the
    // next one of those rather than under what is playing.
    if (key == "resampling" || key == "halveDSDVolume" || key == "outputDSDAsDoP" ||
        key == "suspendOutputOnPause") {
        return {Effect::None, Applies::NextDeviceOpen};
    }

    if (key == "GraphicEQtrackgenre") {
        return {Effect::GenreEqualizer, Applies::Immediately};
    }
    // The preset row writes the curve into the eq keys, which carry the reload.
    if (key == "GraphicEQpreset") {
        return {Effect::None, Applies::Immediately};
    }

    if (key == "repeat" || key == "shuffle" || key == "alwaysStopAfterCurrent") {
        return {Effect::PlaylistMode, Applies::Immediately};
    }
    if (key == "volume") {
        return {Effect::Volume, Applies::Immediately};
    }
    if (key == "panelFollowMode") {
        return {Effect::RefreshPanels, Applies::Immediately};
    }
    if (key == "floatingMiniWindow") {
        return {Effect::MiniFloating, Applies::Immediately};
    }
    if (key == "enableAudioScrobbler") {
        return {Effect::Scrobbler, Applies::Immediately};
    }
    if (key == "sentryConsented") {
        return {Effect::CrashReporter, Applies::Immediately};
    }

    // Session state rather than preferences. Readable, never written from
    // outside: what the last session did is not something a peer gets to revise.
    if (key == "settingsSchemaVersion" || key == "UserDefaultURLsKey" ||
        key == "sentryAskedConsent" || key == "lastPlaybackStatus" ||
        key == "miniMode" || key == "trayHideAnnounced" || key == "outputDevice" ||
        key == "trashAskedConsent") {
        return {Effect::Internal, Applies::Immediately};
    }

    // Read while a folder is being walked, so the next scan is when they matter.
    if (key == "readCueSheetsInFolders" || key == "readPlaylistsInFolders" ||
        key == "skipAppleDoubleFiles") {
        return {Effect::None, Applies::NextScan};
    }

    // Read when a synthesised track is opened, and the MIDI backend along with
    // it. Changing a SoundFont does not re-render what is already playing.
    if (key == "synthSampleRate" || key == "synthDefaultSeconds" ||
        key == "synthDefaultFadeSeconds" || key == "synthDefaultLoopCount" ||
        key == "midiPlugin" || key == "midiRomPath" || key == "soundFontPath") {
        return {Effect::None, Applies::NextTrack};
    }

    // Read when the HTTP source opens a stream.
    if (key == "httpStreamingBufferSize") {
        return {Effect::None, Applies::NextTrack};
    }

    // Read at the point of use: the next track change decides what to do about
    // these two, and the transport asks when it gets there.
    if (key == "keepPlayingWhileSkipping" || key == "selectionFollowsPlayback") {
        return {Effect::None, Applies::Immediately};
    }
    // Read once, during startup.
    if (key == "resumePlaybackOnStartup") {
        return {Effect::None, Applies::NextLaunch};
    }

    // The catalogue is installed before the first window, and wx has no way to
    // restate a window's strings afterwards. The picker says so.
    if (key == "language" || key == "widgetStyle" || key == "closeToTray") {
        return {Effect::None, Applies::NextLaunch};
    }

    // Read when a notification is about to be posted.
    if (key.starts_with("notifications.")) {
        return {Effect::None, Applies::Immediately};
    }

    return {Effect::None, Applies::Immediately};
}

}  // namespace xpcog::app
