// What has to happen when a setting changes, as a table rather than as a chain
// of ifs inside a window.
//
// The problem this solves has already bitten once. Most settings are read when
// they are needed, so writing one is enough. A minority are *held* somewhere --
// in the engine's DSP chain, in the playlist's own state, in a slider -- and for
// those, writing the setting and stopping there leaves the value stored and
// nothing acting on it. `alwaysStopAfterCurrent` was exactly that: the box
// stayed ticked, the setting was stored, and playback carried on to the next
// track until the following launch. Repeat and Shuffle had the same defect and
// escaped notice only because the Order menu happened to set them on the
// playlist directly as well.
//
// While the preferences dialog was the only writer, the chain in
// MainFrame::onSettingChanged was where that knowledge lived, and a setting
// added without a branch there was a bug nobody could see. The REST remote
// control is a second writer, so the knowledge moves into a pure function that
// can be tested against Settings::all() -- and it is: xpcog-app-tests asserts
// that *every* key maps to a deliberate answer, with an explicit allow-list for
// the ones that really do need nothing. Adding a setting without deciding this
// is now a test failure rather than a silent inert row.
//
// The second job is honesty. The API reports when a write takes effect, and
// "immediately" and "on the next track" are not the same promise. Both come from
// here, so the answer the API gives and the action the window takes cannot
// disagree.

#pragma once

#include <string_view>

namespace xpcog::app {

/// What the interface has to do about a changed setting.
enum class Effect {
    /// Nothing. The value is read at the point of use -- when a track opens,
    /// when a folder is scanned, when a dialog is built -- so storing it is the
    /// whole of the work.
    None,
    ReloadDsp,        ///< The engine holds the DSP chain; make it re-read.
    ReopenOutput,     ///< Move the running stream to the device now named.
    RefreshSpeed,     ///< The speed popup shows a stale number until told.
    RefreshPanels,    ///< Info and Lyrics decide what to show from this.
    RefreshSpectrum,  ///< The analyser holds its own copy of these.
    GenreEqualizer,   ///< Apply the playing track's genre curve at once.
    PlaylistMode,     ///< Repeat, shuffle and stop-after live on the Playlist.
    Volume,           ///< Engine, slider and the OS's now-playing entry.
    MiniFloating,     ///< The mini window's always-on-top flag.
    Scrobbler,        ///< Start or stop the submission worker.
    CrashReporter,    ///< Start or close the SDK, in both directions.
    RestartRemote,    ///< Rebind the REST server, so a port change needs no relaunch.
    Internal,         ///< Session state, not a preference. Never written remotely.
};

/// When a write to this key becomes audible or visible.
///
/// Five answers rather than the two an API would like, because the truth has
/// five shapes. `nextTrack` in particular is not a hedge: ReplayGain is applied
/// in AudioEngine::applyReplayGain() when a track opens, and reloadDsp() does not
/// reach it -- so a volumeScaling change genuinely cannot move what is already
/// playing.
enum class Applies {
    Immediately,
    NextTrack,
    NextDeviceOpen,
    NextScan,
    NextLaunch,
};

struct SettingEffect {
    Effect  effect  = Effect::None;
    Applies applies = Applies::Immediately;
};

/// What to do about `key`, for every key in settings.def.
///
/// An unknown key answers `{None, Immediately}` -- there is nothing useful to do
/// about a key this build does not have, and the test is what keeps that from
/// quietly covering a real one.
[[nodiscard]] SettingEffect effectOf(std::string_view key);

/// The wire spelling, for the API's `appliesFrom` field. Untranslated: this is
/// protocol, not interface text.
[[nodiscard]] std::string_view appliesFromName(Applies applies);

}  // namespace xpcog::app
