// The transport's position bar.
//
// A plain QSlider is wrong for seeking in two ways that both read as the control
// being broken:
//
//  * Clicking the groove pages by a step instead of jumping to where you
//    clicked. On a seek bar, clicking two thirds along means "go two thirds in".
//  * The time readout does not follow the handle during a drag, so there is
//    nothing to aim with until after you let go.
//
// Both are fixed here rather than in the window, so the behaviour belongs to the
// widget and cannot be half-wired by a second user of it.

#pragma once

#include <QSlider>

namespace xpcog::app {

class SeekSlider : public QSlider {
    Q_OBJECT

public:
    explicit SeekSlider(QWidget* parent = nullptr);

    /// True while the user holds the handle. The window uses this to stop
    /// position updates fighting the cursor.
    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

signals:
    /// The user let go. Carries seconds, so the window does not have to know
    /// how the slider is scaled.
    void seekRequested(double seconds);

    /// The handle moved while held, for the clock to follow.
    void scrubbed(double seconds);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int valueAt(int x) const;

    bool scrubbing_ = false;
};

}  // namespace xpcog::app
