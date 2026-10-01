#pragma once

#include "WinRT.hpp"

namespace xpcog {
class PluginRegistry;
}

namespace xpcog::winui {

/// Fills `box` with the About dialog's body: the build, and three pages --
/// About, the formats this build plays, and the licences of what it is built
/// from. The wx AboutDialog's content, in a ContentDialog the caller shows.
///
/// The formats are read from the registry rather than written down, so they
/// say what *this* build does; the licences are uicore's shared table, with
/// this frontend's own rows.
void fillAboutDialog(const mux::Controls::ContentDialog& box, const PluginRegistry& registry);

}  // namespace xpcog::winui
