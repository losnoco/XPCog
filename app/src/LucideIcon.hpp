// Lucide glyphs, in the system's colour.
//
// The transport toolbar had no icons at all before these: Previous, Play/Pause,
// Stop and Next drew as text labels, which is legible and looks like a settings
// dialog rather than a player. Lucide (https://lucide.dev, ISC) is a stroked 24px
// set with a consistent weight, which is what a row of transport buttons needs to
// read as one control rather than four.
//
// The colour is the point of this file. Lucide's SVGs stroke with
// `stroke="currentColor"` -- a CSS keyword meaning "whatever colour the
// surrounding text is". There is no surrounding text in a bitmap, and nanosvg --
// which is what wxBitmapBundle::FromSVG parses with -- has no CSS keyword
// handling at all, so left alone it falls through to a wrong default. A dark
// theme would get a toolbar of black on near-black.
//
// So the substitution happens in the SVG text before it is parsed, which is also
// why these are stored as SVG source rather than baked to PNGs: the colour is not
// knowable until the system appearance is.
//
// That much is unchanged from the Qt version, which had to do exactly the same
// thing for exactly the same reason. What changed is the disabled state. Qt drew
// the same pixmap at 35% painter opacity; wxBitmapBundle has no painter and no
// Normal/Disabled pair, so the fade is injected into the SVG as a
// `stroke-opacity` attribute on the root element and rendered as a second bundle.
// stroke-opacity rather than opacity because these icons are stroke-only and it
// is the attribute nanosvg applies most directly.

#pragma once

#include <wx/bmpbndl.h>
#include <wx/colour.h>

#include <string>
#include <string_view>
#include <vector>

namespace xpcog::app {

/// The named Lucide icon -- `"play"`, `"skip-back"` -- stroked in the system's
/// button-text colour.
///
/// A name with no file behind it returns an empty bundle, which draws as nothing
/// rather than as a placeholder. That is a silent failure, so the test asserts
/// every name in lucideIconNames() is present.
[[nodiscard]] wxBitmapBundle lucideIcon(std::string_view name);

/// The dimmed variant, for a disabled button. wx has no disabled slot on a
/// bundle, so this is a separate bundle the caller hands to SetBitmapDisabled().
[[nodiscard]] wxBitmapBundle lucideIconDisabled(std::string_view name);

/// The same in an explicit colour, for anywhere the system's is not the answer.
[[nodiscard]] wxBitmapBundle lucideIcon(std::string_view name, const wxColour& colour,
                                        double opacity = 1.0);

/// Every icon name this build expects to find. The list the test walks, and the
/// reason a typo in a call is caught by a test rather than by noticing a blank
/// button.
[[nodiscard]] std::vector<std::string> lucideIconNames();

/// The resource path a name maps to, so a test can check the file is there
/// without building a bitmap -- which needs an application object.
[[nodiscard]] std::string lucideIconPath(std::string_view name);

/// Drops every cached bundle, so the next request re-strokes in the current
/// colour. Called when the system appearance changes; without it a switch to
/// dark mode leaves every icon in the light theme's colour.
void forgetLucideIcons();

}  // namespace xpcog::app
