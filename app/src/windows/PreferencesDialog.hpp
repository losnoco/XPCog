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
//    resampler quality picker -- are described by a small table, because a
//    combo box with the right labels is worth writing out.
//  * Everything else in settings.def gets a generated row. That is the payoff
//    of the X-macro: a setting added to one file appears here with no further
//    work, and cannot silently become unreachable from the UI the way a Cog
//    setting with no pane does.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <QDialog>

#include <functional>
#include <string>

class QWidget;

namespace xpcog::app {

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(Settings& settings, QWidget* parent = nullptr);

signals:
    /// A setting changed. The engine reads most settings live, but the ones
    /// that only take effect on the next track are worth saying so about.
    void settingChanged(const QString& key);

private:
    // One pane per screen, named and ordered after Cog's own
    // (Preferences/Preferences/GeneralPreferencesPlugin.m:44) minus the panes
    // that have nothing to hold here: Hot Keys, Notifications and Rubber Band
    // are unported, and Cog's General pane is sandbox paths and crash
    // reporting. Spectrum is ours -- Cog keeps its three spectrum rows on
    // Appearance, and there are eight here.
    [[nodiscard]] QWidget* buildPlaylistPane();
    [[nodiscard]] QWidget* buildOutputPane();
    [[nodiscard]] QWidget* buildMidiPane();
    [[nodiscard]] QWidget* buildAppearancePane();
    [[nodiscard]] QWidget* buildSpectrumPane();

    /// A labelled swatch button that opens a colour picker and reports the choice
    /// back as "#rrggbb".
    ///
    /// A helper because there are two of these and they must behave identically --
    /// the interesting part is what happens to an unparseable stored value, and
    /// having that answered once is the point.
    [[nodiscard]] QWidget* colorRow(const QString& label, const std::string& stored,
                                    std::function<void(const std::string&)> store);
    [[nodiscard]] QWidget* buildAdvancedPane();

    /// What the curated rows call when a value changes. Handed to them rather
    /// than reached for, so nothing outside this class emits its signals.
    [[nodiscard]] std::function<void(const char*)> changeNotifier();

    Settings& settings_;
};

}  // namespace xpcog::app
