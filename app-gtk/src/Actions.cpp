#include "Actions.hpp"

#include "Accelerators.hpp"

#include <cstring>

namespace xpcog::gtk {

using app::CommandId;
using app::ItemKind;

namespace {

/// The kind the menu table gives `id`, or Normal for a command with no row --
/// the playlist commands, which are all plain.
ItemKind kindOf(CommandId id) {
    for (const app::MenuItem& item : app::menuLayout()) {
        if (item.id == id) {
            return item.kind;
        }
    }
    return ItemKind::Normal;
}

}  // namespace

Actions::Actions(GActionMap* map, Handler handler) : handler_(std::move(handler)) {
    for (const CommandId id : app::allCommands()) {
        if (!offeredHere(id)) {
            continue;
        }
        const app::CommandAction spec = app::commandAction(id);
        if (byName_.count(spec.name) != 0) {
            // The second, third and fourth members of a radio group share the
            // first one's action.
            continue;
        }

        GSimpleAction* action = nullptr;
        if (spec.target != nullptr) {
            // A radio group: string state, string parameter, and the first
            // member's target as the initial state until the window says
            // otherwise.
            action = g_simple_action_new_stateful(spec.name, G_VARIANT_TYPE_STRING,
                                                  g_variant_new_string(spec.target));
        } else if (kindOf(id) == ItemKind::Check) {
            action = g_simple_action_new_stateful(spec.name, nullptr,
                                                  g_variant_new_boolean(FALSE));
        } else {
            action = g_simple_action_new(spec.name, nullptr);
        }

        handlers_.push_back(connect<void(GSimpleAction*, GVariant*)>(
            action, "activate",
            [this](GSimpleAction* a, GVariant* parameter) { onActivate(a, parameter); }));

        g_action_map_add_action(map, G_ACTION(action));
        byName_[spec.name] = action;
        owned_.push_back(GObjectPtr<GSimpleAction>::adopt(action));
    }
}

Actions::~Actions() = default;

void Actions::onActivate(GSimpleAction* action, GVariant* parameter) {
    const char* name = g_action_get_name(G_ACTION(action));
    // A radio group's parameter says which member; anything else is the one
    // command that owns the name.
    const char* target = (parameter != nullptr && g_variant_is_of_type(parameter, G_VARIANT_TYPE_STRING))
                             ? g_variant_get_string(parameter, nullptr)
                             : nullptr;
    for (const CommandId id : app::allCommands()) {
        if (!offeredHere(id)) {
            continue;
        }
        const app::CommandAction spec = app::commandAction(id);
        if (std::strcmp(spec.name, name) != 0) {
            continue;
        }
        if (spec.target == nullptr || (target != nullptr && std::strcmp(spec.target, target) == 0)) {
            handler_(id);
            return;
        }
    }
}

GSimpleAction* Actions::actionFor(CommandId id) const {
    const auto found = byName_.find(app::commandAction(id).name);
    return found == byName_.end() ? nullptr : found->second;
}

void Actions::setEnabled(CommandId id, bool enabled) {
    if (GSimpleAction* action = actionFor(id)) {
        g_simple_action_set_enabled(action, enabled ? TRUE : FALSE);
    }
}

void Actions::setChecked(CommandId id, bool checked) {
    if (GSimpleAction* action = actionFor(id)) {
        g_simple_action_set_state(action, g_variant_new_boolean(checked ? TRUE : FALSE));
    }
}

void Actions::setChoice(CommandId id) {
    const app::CommandAction spec = app::commandAction(id);
    if (spec.target == nullptr) {
        return;
    }
    if (GSimpleAction* action = actionFor(id)) {
        g_simple_action_set_state(action, g_variant_new_string(spec.target));
    }
}

}  // namespace xpcog::gtk
