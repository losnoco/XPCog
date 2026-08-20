// The application icon.
//
// One function rather than a bundle built at each of the four call sites (the
// frame, the tray, the mini player, the about box), because the thing that goes
// wrong is not "no icon" -- that is obvious -- but two of them disagreeing about
// which sizes exist, so the tray looks blurry while the window looks fine.
//
// Assembled from a set of separately-resized PNGs rather than one large one. A
// toolkit will happily scale a 256px bitmap down to 16, and for this artwork it
// looks muddy doing it: the gear teeth are one pixel wide at that size, which is
// exactly where a smooth filter turns detail into grey. The sizes come out of
// app/icons/make-icons.py, which is also what documents why there are two
// masters.

#pragma once

#include <wx/bmpbndl.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>

#include <string>
#include <vector>

namespace xpcog::app {

/// The icon, carrying every size the resource holds.
[[nodiscard]] wxBitmapBundle applicationIcon();

/// The same as a wxIcon at one size, for the surfaces that still take one --
/// the tray, most obviously.
[[nodiscard]] wxIcon applicationIconAt(int size);

/// Every size as an icon bundle, which is what a frame's SetIcons() takes.
/// A bundle rather than one icon so the window manager picks its own size
/// instead of scaling whichever one it was handed.
[[nodiscard]] wxIconBundle applicationIcons();

/// The sizes applicationIcon() looks for, and where each one lives.
///
/// Exposed so a test can confirm the files are really there. That cannot be done
/// through applicationIcon(): decoding a bitmap needs the image handlers, and
/// therefore an application object, which the tests deliberately do not have.
/// The test reads the embedded bytes instead and checks the same paths this
/// builds from.
[[nodiscard]] std::vector<int> applicationIconSizes();
[[nodiscard]] std::string      applicationIconPath(int size);

}  // namespace xpcog::app
