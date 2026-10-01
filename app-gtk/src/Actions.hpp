// The command table as GActions.
//
// wx binds a command to an integer id that a menu item, a toolbar tool and an
// accelerator all post, and reads its enabled and checked state back every
// idle. GTK binds a command to a *name* that a menu item, a button and a
// shortcut all refer to, and the state is pushed onto the action when it
// changes. Same table, other direction: this installs one GSimpleAction per
// command name -- a plain one for a Normal command, a boolean stateful one for
// a Check, and one string-stateful action per radio group -- and hands every
// activation to one handler with the CommandId, so the window has one switch
// to write, the way MainFrame has one Bind per command.
//
// State stays the window's. GTK would toggle a boolean action by itself when
// it is activated with no handler connected; every action here has one, so
// nothing changes state behind the window's back, and the window says what
// the state is -- from the setting, the pane, the stack -- exactly when it
// would have answered an EVT_UPDATE_UI.

#pragma once

#include "Commands.hpp"
#include "Glib.hpp"

#include <gio/gio.h>

#include <functional>
#include <map>
#include <string>

namespace xpcog::gtk {

class Actions {
public:
    using Handler = std::function<void(app::CommandId)>;

    /// Installs the actions on `map` -- the window, for "win." names -- and
    /// routes every activation to `handler`.
    Actions(GActionMap* map, Handler handler);
    ~Actions();

    Actions(const Actions&)            = delete;
    Actions& operator=(const Actions&) = delete;

    void setEnabled(app::CommandId id, bool enabled);

    /// For a Check command: shows it ticked or not.
    void setChecked(app::CommandId id, bool checked);

    /// For a radio command: makes its group show this one as the choice.
    void setChoice(app::CommandId id);

private:
    void onActivate(GSimpleAction* action, GVariant* parameter);

    /// The action behind `id`'s name. Shared by the members of a radio group.
    [[nodiscard]] GSimpleAction* actionFor(app::CommandId id) const;

    Handler                                handler_;
    std::map<std::string, GSimpleAction*>  byName_;  // borrowed; the map owns them
    std::vector<gulong>                    handlers_;
    std::vector<GObjectPtr<GSimpleAction>> owned_;
};

}  // namespace xpcog::gtk
