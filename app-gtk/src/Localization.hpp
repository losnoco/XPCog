// Which language the interface speaks, for a frontend whose strings come from
// two places.
//
// The C++ side reads uicore's compiled-in catalogue through tr(). The .ui
// files are another matter: GtkBuilder translates their strings through the C
// library's gettext, which reads .mo files from a directory, and has no way
// to be handed a table. So the same catalogue is written out as the .mo image
// Translations.hpp assembles, into the cache directory, and the domain is
// bound there. One table, two readers, and a test that both agree.
//
// The language is chosen once, before the first .ui is built, because gettext
// settles what LANGUAGE means the first time a domain is asked, and a choice
// made after that changes nothing.

#pragma once

#include <string>

namespace xpcog::gtk {

/// Chooses the language from the setting, or from the desktop's list when the
/// setting is empty, installs uicore's lookup for it, and binds the "xpcog"
/// gettext domain to a .mo written for it. Answers the code installed, or an
/// empty string for English -- the msgids as written, which loads nothing.
std::string installTranslations(const std::string& setting);

}  // namespace xpcog::gtk
