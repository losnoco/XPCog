// The application's own button in the taskbar or Dock: a state badge and a
// progress bar drawn over it.
//
// Port of Cog's DockIconController. Two things it shows, and it is worth being
// precise about the second because the obvious guess is wrong:
//
//   * A badge for the playback state -- play, pause, stop.
//   * A progress bar for **loading a playlist**, not for the position within the
//     track. Cog observes an NSProgress published by PlaylistLoader
//     (`progressOverall`), so the bar fills while files are being added and is
//     absent the rest of the time. A progress bar that tracked the seek position
//     would be a different feature that happens to look the same, and it would sit
//     at some arbitrary fraction for the whole of every track.
//
// The Windows implementation is ITaskbarList3. macOS would be NSDockTile, and is
// not written yet -- see docs/PORTING.md; everything else gets the base class,
// which does nothing.
//
// Takes a `WId` rather than a QWidget so this layer stays clear of QtWidgets: it
// links QtGui, and a window handle is all an overlay icon needs.

#pragma once

#include <QObject>
#include <QString>
#include <qwindowdefs.h>

namespace xpcog::platform {

class TaskbarIntegration : public QObject {
    Q_OBJECT

public:
    /// The implementation for this platform, or a do-nothing one where there is
    /// none. Never null, so callers have no branch to forget.
    ///
    /// `window` must be a top-level window and must outlive this object.
    [[nodiscard]] static TaskbarIntegration* create(WId window,
                                                    QObject* parent = nullptr);

    explicit TaskbarIntegration(QObject* parent = nullptr) : QObject(parent) {}
    ~TaskbarIntegration() override = default;

    /// Playing, paused, or neither. Draws the badge.
    virtual void setPlaybackState(bool playing, bool paused) {
        (void)playing;
        (void)paused;
    }

    /// Loading progress, 0 to 1. Shows the bar.
    virtual void setProgress(double fraction) { (void)fraction; }

    /// Nothing is loading. Removes the bar, leaving the badge alone.
    virtual void clearProgress() {}
};

}  // namespace xpcog::platform
