#pragma once

namespace xpcog::winui {

/// Makes Win2D's classes creatable in this process. Before the first one is.
///
/// A packaged app registers Win2D's activatable classes in its manifest and
/// the system finds them; an unpackaged one has no package manifest, and the
/// application-manifest route (an <activatableClass> per class, of which Win2D
/// has dozens) would be a list to keep in step with every Win2D release. So
/// instead C++/WinRT's activation hook is pointed at a function that asks
/// Microsoft.Graphics.Canvas.dll, beside the executable, for anything in its
/// namespace, and hands every other class to the system as before.
void installWin2DActivation();

}  // namespace xpcog::winui
