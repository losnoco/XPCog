// The Roland SC-55's front panel, in step with what is coming out of the
// speakers.
//
// Port of Cog Visualization/SCView.m. The emulator draws the panel itself --
// sc55_lcd_render_screen() composites a state against a photograph of the
// hardware -- so what is left here is when to ask for it and how to get the
// pixels onto a widget.
//
// **When** is the whole difficulty, and it is answered elsewhere: PanelFeed
// holds the states with a position in their track, and this drains them against
// the position the speaker has actually reached. See PanelFeed.hpp for why that
// cannot work the way the spectrum's synchronisation does.
//
// Nothing is produced while this is hidden. showEvent and hideEvent are what
// switch the feed on and off, so the emulator is not comparing its panel
// against the previous state on every sample for a window nobody opened.

#pragma once

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <vector>

class QTimer;

namespace xpcog::app {

class Sc55PanelWidget : public QWidget {
    Q_OBJECT

public:
    /// `position` reports where the speaker has reached in the current track,
    /// in seconds. A callback rather than a controller reference, because that
    /// is the whole of what this needs to know about playback.
    explicit Sc55PanelWidget(std::function<double()> position,
                             QWidget*                parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// Drains everything now due and keeps the newest. Intermediate states are
    /// dropped rather than drawn: they are the panel's own history, and at up to
    /// two hundred a second nobody could read them going past.
    void tick();

    std::function<double()> position_;
    QTimer*                 timer_ = nullptr;

    /// The photograph the emulator composites onto, and the buffer it
    /// composites into. Both are the emulator's shapes, not ours.
    std::vector<std::uint32_t> background_;
    std::vector<std::uint32_t> buffer_;

    /// A view onto `buffer_`, not a copy -- so a repaint that does not follow a
    /// new state costs nothing.
    QImage image_;
    bool   haveFrame_ = false;
};

}  // namespace xpcog::app
