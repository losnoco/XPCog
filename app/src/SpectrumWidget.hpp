// The spectrum display. Replaces Cog's SpectrumViewCG.
//
// Cog ships two of these: SpectrumViewSK draws through SceneKit and SpectrumViewCG
// through Core Graphics. Having a 2D CPU path is therefore Cog's own arrangement,
// not a compromise, and it is the one ported here -- with QPainter standing in for
// Core Graphics, which is very nearly a one-for-one substitution.
//
// The original plan said "pffft + QML islands". Both halves are dropped, and the
// second is worth stating plainly because it has a consequence beyond this file:
// XPCog is a QWidget application throughout, and a QQuickWidget would bring in the
// QML engine, a second UI paradigm, and the Qt Quick runtime -- for a strip of
// rectangles that QPainter draws in a few hundred microseconds. It also settles the
// deployment question left open in docs/PORTING.md: the 39.4 MB of graphics runtime
// windeployqt copies was being kept *in case* this needed Qt Quick. It does not, so
// that can go.
//
// Where the numbers come from is SpectrumAnalyzer, which is Cog's configuration of
// deadbeef's analyser. This class does nothing but draw them and run the clock.

#pragma once

#include "xpcog/core/audio/SpectrumAnalyzer.hpp"

#include <QWidget>

#include <vector>

class QTimer;

namespace xpcog {
class AudioTap;
}

namespace xpcog::app {

class SpectrumWidget : public QWidget {
    Q_OBJECT

public:
    /// `tap` is borrowed and must outlive this widget, which it does: the playback
    /// controller owns both it and, transitively, this window.
    explicit SpectrumWidget(AudioTap& tap, QWidget* parent = nullptr);

    /// The rate the analysis window is taken at. Needed because the band table
    /// depends on it and the widget has no other way to learn it.
    void setSampleRate(double rate);

    /// Starts and stops the repaint clock. Called when playback starts or stops, and
    /// when the panel is shown or hidden -- an invisible widget must not be running a
    /// 4096-point FFT sixty times a second.
    void setActive(bool active);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void tick();

    AudioTap&        tap_;
    SpectrumAnalyzer analyzer_;
    QTimer*          timer_ = nullptr;

    /// The window handed to the analyser each frame. Held rather than allocated per
    /// tick: 4096 floats, sixty times a second, is a pointless amount of churn to
    /// hand the allocator.
    std::vector<float> window_;

    /// Whether playback is running. Separate from the timer, because the timer also
    /// stops when the widget is hidden and the two reasons must not be confused.
    bool playing_ = false;
};

}  // namespace xpcog::app
