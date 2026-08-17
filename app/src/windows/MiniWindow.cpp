#include "MiniWindow.hpp"

#include "ActionRegistry.hpp"
#include "AppIcon.hpp"
#include "PlaybackController.hpp"
#include "SeekSlider.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

#include <cmath>

namespace xpcog::app {
namespace {

/// Seconds to "m:ss". The same shape the main window's clock uses; duplicated
/// rather than shared because it is four lines and hoisting it into a header for
/// two callers costs more attention than it saves.
[[nodiscard]] QString formatClock(double seconds) {
    const auto total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60)
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

/// The transport, in the order the main window's toolbar uses.
constexpr ActionId kTransport[] = {
    ActionId::PlaybackPrevious,
    ActionId::PlaybackPlayPause,
    ActionId::PlaybackStop,
    ActionId::PlaybackNext,
};

}  // namespace

MiniWindow::MiniWindow(const ActionRegistry& actions, PlaybackController& playback,
                       Settings& settings, QWidget* parent)
    : QWidget(parent, Qt::Window), playback_(playback), settings_(settings) {
    setWindowTitle(QStringLiteral("XPCog"));
    setWindowIcon(applicationIcon());

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(4);

    for (const ActionId id : kTransport) {
        QAction* command = actions.action(id);
        if (command == nullptr) {
            continue;
        }
        auto* button = new QToolButton(this);
        button->setDefaultAction(command);
        // Icon-only: the labels are "&Play/Pause" and "Pre&vious", which are menu
        // text and far too wide for a row meant to be small.
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        row->addWidget(button);
    }

    seekBar_ = new SeekSlider(this);
    seekBar_->setEnabled(false);
    row->addWidget(seekBar_, 1);

    clock_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    row->addWidget(clock_);

    volume_ = new QSlider(Qt::Horizontal, this);
    volume_->setRange(0, 100);
    volume_->setMaximumWidth(70);
    row->addWidget(volume_);

    connect(seekBar_, &SeekSlider::seekRequested, this,
            [this](double seconds) { playback_.seek(seconds); });
    connect(seekBar_, &SeekSlider::scrubbed, this, [this](double seconds) {
        clock_->setText(QStringLiteral("%1 / %2")
                            .arg(formatClock(seconds), formatClock(duration_)));
    });
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        const double linear = static_cast<double>(value) / 100.0;
        playback_.setVolume(linear);
        emit volumeChanged(linear);
    });

    // "Always on top" lives in a context menu rather than in the View menu.
    // The mini player has no menu bar of its own -- that is rather the point of it
    // -- and an entry in the main window's View menu would be inert whenever the
    // mini player is not showing, which is most of the time. Right-clicking a
    // compact player is where anyone would look for it.
    setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* floating = new QAction(tr("Always on Top"), this);
    floating->setCheckable(true);
    floating->setChecked(settings_.FloatingMiniWindow());
    connect(floating, &QAction::toggled, this, [this](bool on) { setFloating(on); });
    addAction(floating);

    refreshVolume();
    // Small, and remembered from here on by the caller's saved geometry.
    resize(460, 44);
    setFloating(settings_.FloatingMiniWindow());
}

void MiniWindow::setNowPlaying(const QString& title, const QString& artist) {
    // Into the window title, which is what Cog does -- it sets miniWindow.title and
    // .subtitle rather than putting a label in the window, because on macOS the
    // title bar is the only surface it has. Here the title bar is simply the
    // roomiest place to put text in a window this short.
    if (title.isEmpty() && artist.isEmpty()) {
        setWindowTitle(QStringLiteral("XPCog"));
        return;
    }
    setWindowTitle(artist.isEmpty()
                       ? title
                       : QStringLiteral("%1 — %2").arg(artist, title));
}

void MiniWindow::setPosition(double seconds, double duration) {
    duration_ = duration;
    if (!seekBar_->isSliderDown()) {
        seekBar_->setRange(0, static_cast<int>(std::lround(duration)));
        seekBar_->setValue(static_cast<int>(std::lround(seconds)));
        clock_->setText(QStringLiteral("%1 / %2")
                            .arg(formatClock(seconds), formatClock(duration)));
    }
}

void MiniWindow::setPlaybackState(bool playing, bool paused) {
    (void)paused;  // the Play/Pause action relabels itself; nothing to do here
    seekBar_->setEnabled(playing);
}

void MiniWindow::refreshVolume() {
    // Without signals: this is reading the authoritative value back, and letting it
    // round-trip through valueChanged would write the same number straight back to
    // the controller and re-emit volumeChanged for a change nobody made.
    const QSignalBlocker blocker(volume_);
    volume_->setValue(static_cast<int>(std::lround(playback_.volume() * 100.0)));
}

void MiniWindow::setFloating(bool floating) {
    settings_.setFloatingMiniWindow(floating);
    // Qt::WindowStaysOnTopHint has to be applied to the window flags, which on most
    // platforms recreates the native window -- so anything that was visible has to
    // be shown again afterwards.
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, floating);
    if (wasVisible) {
        show();
    }
}

void MiniWindow::closeEvent(QCloseEvent* event) {
    // Closing the mini player means "back to the full window", not "quit". In a
    // mode-based design that distinction is the whole of the close button's
    // meaning, and getting it wrong exits the application from a window with no
    // other way out.
    event->accept();
    emit dismissed();
}

}  // namespace xpcog::app
