// The application icon.
//
// One function rather than a QIcon constructed at each of the four call sites
// (the application, the window, the tray, the about box), because the thing that
// goes wrong is not "no icon" -- that is obvious -- but two of them disagreeing
// about which sizes exist, so the tray looks blurry while the window looks fine.
//
// Assembled from a set of separately-resized PNGs rather than one large one. Qt
// will happily scale a 256px pixmap down to 16, and for this artwork it looks
// muddy doing it: the gear teeth are one pixel wide at that size, which is
// exactly where a smooth filter turns detail into grey. The sizes come out of
// app/icons/make-icons.py, which is also what documents why there are two
// masters.

#pragma once

#include <QIcon>
#include <QList>
#include <QString>

namespace xpcog::app {

/// The icon, carrying every size the resource holds. Cheap to call: built once
/// and returned by value, which is a refcount copy.
[[nodiscard]] QIcon applicationIcon();

/// The sizes applicationIcon() looks for, and where each one lives.
///
/// Exposed so a test can confirm the files are really there. That cannot be
/// done through applicationIcon(): a QIcon needs a QGuiApplication, which the
/// app tests deliberately do not have -- so the test reads the resource with
/// QImage, which needs no application at all, and checks the same paths this
/// builds from.
[[nodiscard]] QList<int> applicationIconSizes();
[[nodiscard]] QString    applicationIconPath(int size);

}  // namespace xpcog::app
