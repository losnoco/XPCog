#include "Accelerators.hpp"

#include <cctype>
#include <vector>

namespace xpcog::gtk {

std::string gtkAccelerator(std::string_view spelling) {
    if (spelling.empty()) {
        return {};
    }

    std::string modifiers;
    std::string key;

    std::size_t start = 0;
    while (start <= spelling.size()) {
        const std::size_t plus = spelling.find('+', start);
        // A '+' as the last character is the key itself, not a separator.
        const bool last = plus == std::string_view::npos || plus + 1 >= spelling.size();
        const std::string_view part =
            last ? spelling.substr(start) : spelling.substr(start, plus - start);

        if (!last) {
            if (part == "Ctrl") {
                modifiers += "<Control>";
            } else if (part == "Shift") {
                modifiers += "<Shift>";
            } else if (part == "Alt") {
                modifiers += "<Alt>";
            } else {
                // A modifier this table does not use; pass it through so a
                // wrong spelling fails loudly at gtk_accelerator_parse rather
                // than silently as nothing.
                modifiers += "<" + std::string(part) + ">";
            }
            start = plus + 1;
            continue;
        }

        // The key. Named keys are keysym names; a single character is spelled
        // as itself, lower-cased, since GTK matches the unshifted key.
        if (part == "Del") {
            key = "Delete";
        } else if (part == ",") {
            key = "comma";
        } else if (part == ".") {
            key = "period";
        } else if (part == "Left" || part == "Right" || part == "Up" || part == "Down" ||
                   part == "Space" || part == "Return" || part == "Escape" ||
                   part == "Home" || part == "End") {
            key = std::string(part);
        } else if (part.size() == 1) {
            key = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(part[0]))));
        } else {
            key = std::string(part);
        }
        break;
    }

    return modifiers + key;
}

std::string actionName(app::CommandId id) {
    return "win." + std::string(app::commandAction(id).name);
}

std::string detailedActionName(app::CommandId id) {
    const app::CommandAction action = app::commandAction(id);
    std::string              name   = "win." + std::string(action.name);
    if (action.target != nullptr) {
        name += "::";
        name += action.target;
    }
    return name;
}

bool offeredHere(app::CommandId id) { return id != app::CommandId::ViewDockPanes; }

void installAccelerators(GtkApplication* application) {
    for (const app::CommandId id : app::allCommands()) {
        if (!offeredHere(id)) {
            continue;
        }
        const std::string spelled = gtkAccelerator(app::commandAccelerator(id));
        if (spelled.empty()) {
            continue;
        }
        const std::string detailed = detailedActionName(id);
        const char* const accels[] = {spelled.c_str(), nullptr};
        gtk_application_set_accels_for_action(application, detailed.c_str(), accels);
    }

    // The shortcuts dialog's own key: GNOME's, Ctrl+?, for an action the
    // window adds beside the table's.
    const char* const shortcuts[] = {"<Control>question", nullptr};
    gtk_application_set_accels_for_action(application, "win.shortcuts", shortcuts);
}

}  // namespace xpcog::gtk
