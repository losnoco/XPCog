// Preferences. Replaces Cog's eight preference panes.
//
// The screens are Cog's, so someone who knows where a setting lives there finds
// it in the same place here -- including the two placements that look odd until
// you know them: HDCD, ReplayGain and FreeSurround are Output rather than
// Playback, and the default play time, fade and loop count for *every*
// synthesised format are on MIDI, because that is the pane they grew on.
//
// One Cog placement is deliberately not copied. Cog binds the global resampler
// quality on its MIDI pane too, which is where it ends up hidden from anyone
// looking for it; it sits on Output here, with the rest of the output chain.
//
// Two halves, on purpose:
//
//  * The settings that deserve real UI -- a named list of ReplayGain modes, a
//    resampler quality picker -- are described by a small table, because a combo
//    box with the right labels is worth writing out.
//  * Everything else in settings.def gets a generated row. That is the payoff of
//    the X-macro: a setting added to one file appears here with no further work,
//    and cannot silently become unreachable from the UI the way a Cog setting
//    with no pane does.
//
// The second half is now nearly empty, and that is the intended end state rather
// than a sign it is unused. Every setting a listener would look for has a pane
// row; what falls through is one unimplemented option and the session's own
// record of itself, shown but not editable. Its job from here is to catch the
// *next* setting somebody adds, so a new key is visible from the first build
// rather than from the commit that remembers to draw it.
//
// The sidebar-and-pages shape is a wxListbook, which is exactly the widget the
// Qt version was assembling by hand out of a QListWidget and a QStackedWidget.

#pragma once

#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/dialog.h>

#include <functional>
#include <string>

class wxListbook;
class wxWindow;

namespace xpcog::app {

class PreferencesDialog : public wxDialog {
public:
    PreferencesDialog(wxWindow* parent, Settings& settings);

    /// A setting changed. The engine reads most settings live, but the ones that
    /// only take effect on the next track are worth saying so about.
    Signal<std::string> settingChanged;

private:
    // One pane per screen, named and ordered after Cog's own
    // (Preferences/Preferences/GeneralPreferencesPlugin.m:44) minus the panes
    // that have nothing to hold here: Rubber Band is unported, and Hot Keys is
    // not coming -- its seven shortcuts are media keys, which SMTC, MPRIS and
    // MediaPlayer.framework already deliver through platform/, so a pane of key
    // bindings would be a second way to ask for something the OS is already
    // sending. Cog's General pane is sandbox paths and crash reporting, and only
    // the second of those exists here -- the sandbox is a macOS entitlement
    // arrangement with nothing to port. Spectrum is ours -- Cog keeps its three
    // spectrum rows on Appearance, and there are eight here.
    [[nodiscard]] wxWindow* buildPlaylistPane(wxWindow* parent);
    [[nodiscard]] wxWindow* buildOutputPane(wxWindow* parent);
    [[nodiscard]] wxWindow* buildGeneralPane(wxWindow* parent);
    [[nodiscard]] wxWindow* buildNotificationsPane(wxWindow* parent);
    [[nodiscard]] wxWindow* buildMidiPane(wxWindow* parent);
    // Appearance is not built on macOS: its one control is Windows and Linux only,
    // and what would be left is a category holding a single greyed-out paragraph.
#ifndef __WXOSX__
    [[nodiscard]] wxWindow* buildAppearancePane(wxWindow* parent);
#endif
    [[nodiscard]] wxWindow* buildSpectrumPane(wxWindow* parent);
    [[nodiscard]] wxWindow* buildAdvancedPane(wxWindow* parent);

    /// What the curated rows call when a value changes. Handed to them rather
    /// than reached for, so nothing outside this class publishes its signal.
    [[nodiscard]] std::function<void(const char*)> changeNotifier();

    Settings& settings_;
};

}  // namespace xpcog::app
