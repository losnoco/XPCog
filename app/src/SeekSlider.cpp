#include "SeekSlider.hpp"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

namespace xpcog::app {

SeekSlider::SeekSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
    setTracking(true);
    connect(this, &QSlider::valueChanged, this, [this](int value) {
        if (scrubbing_) {
            emit scrubbed(static_cast<double>(value));
        }
    });
}

int SeekSlider::valueAt(int x) const {
    QStyleOptionSlider option;
    initStyleOption(&option);

    // The groove is inset by the handle width, and the handle is positioned by
    // its left edge. Without accounting for both, a click lands progressively
    // further off towards the ends of the bar.
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

    const int span     = groove.width() - handle.width();
    const int position = x - groove.x() - (handle.width() / 2);
    if (span <= 0) {
        return minimum();
    }

    return QStyle::sliderValueFromPosition(minimum(), maximum(), position, span,
                                           option.upsideDown);
}

void SeekSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || maximum() <= minimum()) {
        QSlider::mousePressEvent(event);
        return;
    }

    // Jump to the click, then let the base class take over so the same press
    // continues into a drag without needing a second one.
    scrubbing_ = true;
    setSliderDown(true);
    setValue(valueAt(event->position().toPoint().x()));
    emit scrubbed(static_cast<double>(value()));
    event->accept();
}

void SeekSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (!scrubbing_) {
        QSlider::mouseReleaseEvent(event);
        return;
    }

    setValue(valueAt(event->position().toPoint().x()));
    scrubbing_ = false;
    setSliderDown(false);
    emit seekRequested(static_cast<double>(value()));
    event->accept();
}

}  // namespace xpcog::app
