// Preferences. Replaces Cog's ten SwiftUI panes.
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
    [[nodiscard]] QWidget* buildPlaybackPane();
    [[nodiscard]] QWidget* buildAdvancedPane();

    Settings& settings_;
};

}  // namespace xpcog::app
