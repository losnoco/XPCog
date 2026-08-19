#include "Sc55PanelWidget.hpp"

#include "xpcog/core/audio/PanelFeed.hpp"

#include <api.h>

#include <QFile>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <cstring>

namespace xpcog::app {
namespace {

/// Thirty a second. The panel is a character LCD whose firmware repaints it far
/// faster than that; what is being chosen here is how often a *person* sees a
/// change, and the feed's own 5 ms floor has already thrown away the rest.
constexpr int kRefreshMs = 33;

/// The emulator writes into a fixed 1024-wide buffer whatever the panel's real
/// size is, so the stride and the visible width are different numbers.
constexpr int kStride = lcd_width_max;

[[nodiscard]] std::vector<std::uint32_t> loadBackground() {
    std::vector<std::uint32_t> pixels;
    QFile                      file(QStringLiteral(":/sc55/back.data"));
    if (!file.open(QIODevice::ReadOnly)) {
        return pixels;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != static_cast<qsizetype>(lcd_background_size * sizeof(std::uint32_t))) {
        return pixels;
    }
    pixels.resize(lcd_background_size);
    std::memcpy(pixels.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return pixels;
}

}  // namespace

Sc55PanelWidget::Sc55PanelWidget(std::function<double()> position, QWidget* parent)
    : QWidget(parent), position_(std::move(position)) {
    background_ = loadBackground();
    buffer_.assign(lcd_buffer_size, 0);

    // Format_RGBX8888, and the choice is not cosmetic. lcd_buffer_t is
    // uint32_t*, and on a little-endian machine its bytes come out R, G, B, A --
    // so reading the word as 0xAARRGGBB reverses the visible channels and turns
    // the SC-55's amber panel blue, which is what Cog shows. The *X* matters
    // too: the SC-55's two colour constants carry alpha zero, unlike the
    // JV-880's and unlike the background photograph's, so honouring alpha would
    // draw the panel and leave the characters invisible.
    image_ = QImage(reinterpret_cast<const uchar*>(buffer_.data()), lcd_background_width,
                    lcd_background_height, kStride * static_cast<int>(sizeof(std::uint32_t)),
                    QImage::Format_RGBX8888);

    timer_ = new QTimer(this);
    timer_->setInterval(kRefreshMs);
    connect(timer_, &QTimer::timeout, this, &Sc55PanelWidget::tick);

    setMinimumSize(lcd_background_width / 3, lcd_background_height / 3);
}

QSize Sc55PanelWidget::sizeHint() const {
    return {lcd_background_width / 2, lcd_background_height / 2};
}

void Sc55PanelWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Producing costs the emulator a comparison per rendered sample, so it only
    // happens while this is on screen.
    PanelFeed::instance().setWanted(true);
    timer_->start();
}

void Sc55PanelWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop();
    PanelFeed::instance().setWanted(false);
    haveFrame_ = false;
}

void Sc55PanelWidget::tick() {
    if (background_.empty() || !position_) {
        return;
    }
    // Only the last of a batch is drawn. The others are states the panel passed
    // through since the previous repaint, and at up to two hundred a second
    // they are not something an eye resolves.
    std::optional<PanelFrame> draw;
    if (auto frames = PanelFeed::instance().take(position_()); !frames.empty()) {
        draw = std::move(frames.back());
    } else if (!haveFrame_) {
        // Nothing due yet, and nothing shown yet. Rather than sit blank until
        // the speaker catches up with a decoder that is seconds ahead, show the
        // nearest state there is; the next drained frame replaces it and every
        // one after that is on time. See PanelFeed::peekEarliest().
        draw = PanelFeed::instance().peekEarliest();
        if (!draw) {
            // The explanation drawn in its place can change without any frame
            // ever arriving, so it is still worth a repaint.
            update();
            return;
        }
    } else {
        return;
    }

    if (draw->state.size() != sc55_lcd_state_size()) {
        return;
    }
    sc55_lcd_render_screen(background_.data(), buffer_.data(), draw->state.data(),
                           draw->state.size());
    haveFrame_ = true;
    update();
}

void Sc55PanelWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    if (!haveFrame_) {
        // An empty panel has two completely different causes and they look the
        // same, so it says which. "Nothing has been produced" means the track
        // is not playing on a machine that has a front panel -- the OPL3 has
        // none -- and no amount of waiting will change that.
        painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
        const QString message =
            PanelFeed::instance().producing()
                ? tr("Waiting for the panel…")
                : tr("Nothing is playing on the SC-55.\n\nChoose it under "
                     "Preferences → MIDI; the OPL3 synthesisers have no display.");
        painter.drawText(rect().adjusted(12, 12, -12, -12),
                         Qt::AlignCenter | Qt::TextWordWrap, message);
        return;
    }

    // Aspect preserved: the panel is a photograph of a real object, and a
    // stretched one looks like a mistake rather than a design.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QSize scaled = image_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2),
                       scaled);
    painter.drawImage(target, image_);
}

}  // namespace xpcog::app
