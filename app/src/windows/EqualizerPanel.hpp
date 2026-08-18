// The 31-band equaliser, as a panel rather than a preferences pane.
//
// It began life inside PreferencesDialog, which was the wrong home for it. A
// preferences dialog is modal-ish and something you close: everything else in
// there is set once and forgotten, whereas an equaliser is played *with* --
// adjusted while listening, against audio you can hear change. Behind a dialog
// that means opening preferences, finding the pane, dragging a slider, and
// having the thing you are judging half-hidden behind a window.
//
// Cog agrees, and always did: its equaliser is an EqualizerWindowController with
// a window of its own, not one of its ten preference panes. A dock is that with
// the window management already done -- it remembers its place and size through
// the main window's saveState(), and it can still be torn off into a real
// floating window by anyone who wants one.

#pragma once

#include "xpcog/core/Settings.hpp"

#include <QList>
#include <QString>
#include <QWidget>

class QHBoxLayout;
class QSlider;

namespace xpcog::app {

class EqualizerPanel : public QWidget {
    Q_OBJECT

public:
    explicit EqualizerPanel(Settings& settings, QWidget* parent = nullptr);

signals:
    /// A band moved. Named the same as PreferencesDialog's signal and carrying
    /// the same setting keys, so MainWindow's handler is shared: the engine has
    /// to be told to re-read the chain either way.
    void settingChanged(const QString& key);

public slots:
    /// Every band and the preamp back to 0 dB, which makes the equaliser
    /// bit-transparent again -- a flat chain is skipped rather than run.
    void flatten();

private:
    /// One labelled column. Reads and writes by settings key, so nothing here
    /// needs to know 31 accessor names and the band-to-key pairing stays where
    /// the DSP defines it.
    QSlider* addBand(QHBoxLayout* columns, const QString& caption, const char* key);

    Settings&       settings_;
    QList<QSlider*> sliders_;
};

}  // namespace xpcog::app
