// The one place CommandId and wxWidgets have to agree.
//
// Commands.hpp writes the first eight ids out as integers, because it names no
// toolkit and both frontends read it. They are not arbitrary integers: they are
// wx's own standard ids, and they have to stay that way, because wxOSX
// *relocates* items carrying wxID_PREFERENCES, wxID_ABOUT and wxID_EXIT into the
// macOS application menu -- which is what QAction::setMenuRole() used to do by
// hand -- and it keys that on the menu item's own id.
//
// Nothing else in this file. It exists so that a wx which renumbered them would
// fail to build here, on all three platforms CI covers, rather than quietly
// putting Preferences in the File menu on macOS and nowhere else. That is the
// same trade cmake/CheckNoToolkit.cmake makes: a rule nobody has to remember.

#include "Commands.hpp"

#include <wx/defs.h>

namespace xpcog::app {

static_assert(static_cast<int>(FileOpen) == static_cast<int>(wxID_OPEN));
static_assert(static_cast<int>(FileSavePlaylist) == static_cast<int>(wxID_SAVEAS));
static_assert(static_cast<int>(FilePreferences) == static_cast<int>(wxID_PREFERENCES));
static_assert(static_cast<int>(FileQuit) == static_cast<int>(wxID_EXIT));
static_assert(static_cast<int>(EditUndo) == static_cast<int>(wxID_UNDO));
static_assert(static_cast<int>(EditRedo) == static_cast<int>(wxID_REDO));
static_assert(static_cast<int>(EditSelectAll) == static_cast<int>(wxID_SELECTALL));
static_assert(static_cast<int>(HelpAbout) == static_cast<int>(wxID_ABOUT));

// And that the rest still start above everything wx dispatches on its own, so
// nothing this application invents can collide with a stock id. Greater than,
// not equal to wxID_HIGHEST + 1: 3.3 moved wxID_HIGHEST from 5999 to 6000 and
// changed what it means, and an exact match broke the build on the first wx
// that did -- which is precisely what this line is for, but the rule it
// guards is "clear of wx's range", not "one past it".
static_assert(static_cast<int>(FileOpenFolder) > static_cast<int>(wxID_HIGHEST));

}  // namespace xpcog::app
