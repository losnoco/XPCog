// Lucide glyphs, in the palette's colour.
//
// The transport toolbar had no icons at all: Previous, Play/Pause, Stop and Next
// drew as text labels, which is legible and looks like a settings dialog rather
// than a player. Lucide (https://lucide.dev, ISC) is a stroked 24px set with a
// consistent weight, which is what a row of transport buttons needs to read as
// one control rather than four.
//
// The colour is the point of this file. Lucide's SVGs stroke with
// `stroke="currentColor"` -- a CSS keyword that means "whatever colour the
// surrounding text is". There is no surrounding text in a QIcon, so a renderer
// left to itself paints them black, and a dark theme gets a toolbar of black on
// near-black. The substitution here is what makes them follow the palette, and
// it is also why the icons are stored as SVG source rather than baked to PNGs:
// the colour is not knowable until the style is.
//
// Qt6::Svg rather than the qsvg image-format plugin, which would also load these
// but leaves `currentColor` to whatever its own default is. Substituting in the
// text before it is parsed keeps the answer ours.

#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace xpcog::app {

/// The named Lucide icon -- `"play"`, `"skip-back"` -- stroked in the current
/// palette's button-text colour, with a dimmed variant for the disabled state.
///
/// A name with no file behind it returns a null QIcon, which draws as nothing
/// rather than as a placeholder. That is a silent failure, so
/// tests/app/test_lucideicon.cpp asserts every name used here is present.
[[nodiscard]] QIcon lucideIcon(const QString& name);

/// The same in an explicit colour, for anywhere the palette is not the answer.
[[nodiscard]] QIcon lucideIcon(const QString& name, const QColor& colour);

/// Every icon name this build expects to find in the resource. The list the
/// test walks, and the reason a typo in a call is caught at build time rather
/// than by noticing a blank button.
[[nodiscard]] QStringList lucideIconNames();

}  // namespace xpcog::app
