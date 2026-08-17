#include "SpectrumWidget.hpp"

#include "xpcog/core/audio/AudioTap.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
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

void SpectrumWidget::applySettings(const Settings& settings) {
    // Invalid colour text keeps whatever was there, rather than falling back to a
    // default QColor -- which is black, and a black bar on a near-black background
    // is indistinguishable from the spectrum having stopped working. QColor accepts
    // "#rgb", "#rrggbb" and the SVG colour names, so a hand-edited settings file has
    // a fair chance of meaning what it says.
    if (const QColor bar(QString::fromStdString(settings.SpectrumBarColor()));
        bar.isValid()) {
        barColor_ = bar;
    }
    if (const QColor peak(QString::fromStdString(settings.SpectrumDotColor()));
        peak.isValid()) {
        peakColor_ = peak;
    }
    showPeaks_ = settings.SpectrumShowPeaks();

    analyzer_.setFloorDb(settings.SpectrumFloorDb());
    analyzer_.setMode(settings.SpectrumFreqMode()
                          ? SpectrumAnalyzer::Mode::Frequencies
                          : SpectrumAnalyzer::Mode::NoteBands);
    updateFrequencyBandCount();
    update();
}

void SpectrumWidget::updateFrequencyBandCount() {
    if (analyzer_.mode() != SpectrumAnalyzer::Mode::Frequencies) {
        return;
    }
    analyzer_.setFrequencyBandCount(
        static_cast<std::size_t>(std::max(1, width() / kFrequencyBarPitch)));
}

void SpectrumWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Only matters in Frequencies mode, where the bar count follows the width.
    // NoteBands has a fixed series and ignores it.
    updateFrequencyBandCount();
}

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
    // It has to follow the *configured* floor rather than a fixed -80, or the grid
    // would quietly start lying the moment anyone changed it.
    const double floorDb = analyzer_.floorDb();
    painter.setPen(QColor(255, 255, 255, 22));
    for (const int decibels : kGridLinesDb) {
        if (static_cast<double>(decibels) <= floorDb) {
            continue;  // below the floor, so off the bottom of the display
        }
        const double level = (static_cast<double>(decibels) - floorDb) / -floorDb;
        const auto   y     = static_cast<int>(std::lround(height - (level * height)));
        painter.drawLine(0, y, this->width(), y);
    }

    const double slot     = width / static_cast<double>(bands.size());
    const double gap      = slot / kGapDenominator;
    const double barWidth = std::max(1.0, slot - gap);

    // Bottom-to-top gradient over the whole height rather than per bar, so a tall bar
    // and a short one agree about what a given height means -- per-bar gradients make
    // every bar look equally loud at its own tip.
    // Derived from the chosen bar colour rather than a fixed ramp: the colour is a
    // setting now, so anything hard-coded here would ignore it. Darker at the bottom
    // and lighter at the top keeps the shape readable without inventing hues nobody
    // picked.
    QLinearGradient gradient(0, static_cast<int>(height), 0, 0);
    gradient.setColorAt(0.0, barColor_.darker(160));
    gradient.setColorAt(1.0, barColor_.lighter(125));
    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);

    for (std::size_t band = 0; band < bands.size(); ++band) {
        const double level = static_cast<double>(bands[band]);
        if (level <= 0.0) {
            continue;
        }
        const double x   = static_cast<double>(band) * slot;
        const double top = height - (level * height);
        painter.drawRect(QRectF(x, top, barWidth, height - top));
    }

    // The peak markers last, over the bars. Cog draws these as a one-pixel line in
    // its own colour -- spectrumDotColor -- and so does this.
    if (!showPeaks_) {
        return;
    }
    painter.setPen(peakColor_);
    for (std::size_t band = 0; band < peaks.size(); ++band) {
        const double peak = static_cast<double>(peaks[band]);
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
