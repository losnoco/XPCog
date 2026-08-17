#include "SpectrumWidget.hpp"

#include "xpcog/core/audio/AudioTap.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace xpcog::app {
namespace {

/// 60 Hz. Cog redraws on a display link; a timer is the portable equivalent and the
/// difference is not visible on a bar chart.
constexpr int kFrameIntervalMs = 16;

/// Cog divides the space between bars by this (bar_gap_denominator = 3): a third of
/// each slot is gap. Reproduced rather than guessed, because it is what makes a
/// spectrum look like Cog's rather than like a solid block.
constexpr int kGapDenominator = 3;

/// The dB gridlines Cog labels, drawn behind the bars.
constexpr int kGridLinesDb[] = {-10, -20, -30, -40, -50, -60, -70};

}  // namespace

SpectrumWidget::SpectrumWidget(AudioTap& tap, QWidget* parent)
    : QWidget(parent), tap_(tap), window_(SpectrumAnalyzer::kWindowFrames, 0.0F) {
    setObjectName(QStringLiteral("spectrum"));
    // Its own background, painted by paintEvent. Without this the widget inherits
    // whatever the style draws and the bars sit on a light panel in a dark theme.
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);

    timer_ = new QTimer(this);
    timer_->setInterval(kFrameIntervalMs);
    connect(timer_, &QTimer::timeout, this, &SpectrumWidget::tick);
}

void SpectrumWidget::setSampleRate(double rate) { analyzer_.prepare(rate); }

void SpectrumWidget::setActive(bool active) {
    playing_ = active;
    if (!active) {
        analyzer_.reset();
        update();
    }
    // Visible *and* playing, or the clock stops. Either condition alone is a reason
    // not to be running a 4096-point transform sixty times a second.
    if (active && isVisible()) {
        timer_->start();
    } else {
        timer_->stop();
    }
}

void SpectrumWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (playing_) {
        timer_->start();
    }
}

void SpectrumWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop();
}

QSize SpectrumWidget::sizeHint() const { return {400, 120}; }
QSize SpectrumWidget::minimumSizeHint() const { return {120, 48}; }

void SpectrumWidget::tick() {
    if (!tap_.readLatest(window_.data(), window_.size())) {
        return;  // nothing has played yet
    }
    analyzer_.analyze(window_.data(), window_.size());
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor(18, 18, 20));

    const std::vector<float>& bands = analyzer_.bands();
    const std::vector<float>& peaks = analyzer_.peaks();
    if (bands.empty()) {
        return;
    }

    const auto width  = static_cast<double>(this->width());
    const auto height = static_cast<double>(this->height());

    // The dB grid, behind everything. Positioned by the same normalisation the bars
    // use, so a line labelled -40 dB really is where a -40 dB bar reaches.
    painter.setPen(QColor(255, 255, 255, 22));
    for (const int decibels : kGridLinesDb) {
        const double level = (decibels - SpectrumAnalyzer::kFloorDb) /
                             -SpectrumAnalyzer::kFloorDb;
        const auto y = static_cast<int>(std::lround(height - (level * height)));
        painter.drawLine(0, y, this->width(), y);
    }

    const double slot     = width / static_cast<double>(bands.size());
    const double gap      = slot / kGapDenominator;
    const double barWidth = std::max(1.0, slot - gap);

    // Bottom-to-top gradient over the whole height rather than per bar, so a tall bar
    // and a short one agree about what a given height means -- per-bar gradients make
    // every bar look equally loud at its own tip.
    QLinearGradient gradient(0, static_cast<int>(height), 0, 0);
    gradient.setColorAt(0.0, QColor(64, 160, 255));
    gradient.setColorAt(0.6, QColor(96, 220, 160));
    gradient.setColorAt(1.0, QColor(255, 208, 64));
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const double level = bands[band];
        if (level <= 0.0) {
            continue;
        }
        const double x   = static_cast<double>(band) * slot;
        const double top = height - (level * height);
        painter.drawRect(QRectF(x, top, barWidth, height - top));
    }

    // The peak markers last, over the bars. Cog draws these as a one-pixel line in
    // its own colour; the same idea, in a colour that reads on the gradient.
    painter.setPen(QColor(255, 255, 255, 190));
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const double peak = peaks[band];
        if (peak <= 0.0) {
            continue;
        }
        const double x = static_cast<double>(band) * slot;
        const auto   y = static_cast<int>(std::lround(height - (peak * height)));
        painter.drawLine(static_cast<int>(std::lround(x)), y,
                         static_cast<int>(std::lround(x + barWidth)), y);
    }
}

}  // namespace xpcog::app
