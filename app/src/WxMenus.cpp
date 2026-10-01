#include "WxMenus.hpp"

#include "Text.hpp"

#include <wx/accel.h>
#include <wx/menu.h>

#include <memory>
#include <string_view>

namespace xpcog::app {
namespace {

void appendItem(wxMenu& menu, const MenuItem& item) {
    if (item.separatorBefore) {
        menu.AppendSeparator();
    }

    wxString label = trUtf8(item.label);
    if (*item.accelerator != '\0') {
        label += "\t";
        label += wxString::FromAscii(item.accelerator);
    }
    menu.Append(item.id, label, wxEmptyString, toWxItemKind(item.kind));
}

}  // namespace

wxItemKind toWxItemKind(ItemKind kind) {
    switch (kind) {
        case ItemKind::Check: return wxITEM_CHECK;
        case ItemKind::Radio: return wxITEM_RADIO;
        case ItemKind::Normal: break;
    }
    return wxITEM_NORMAL;
}

wxString commandTooltip(CommandId id) {
    // Through the two public accessors rather than the table directly, so a
    // command with no menu row gives an empty tooltip here and an empty label
    // there rather than the two disagreeing.
    const wxString label = toWx(commandLabel(id));
    if (label.empty()) {
        return {};
    }

    const std::string_view accelerator = commandAccelerator(id);
    if (accelerator.empty()) {
        return label;
    }

    // Through wxAcceleratorEntry rather than appended as written. The table
    // spells every shortcut `Ctrl`, which wx turns into Cmd when it builds the
    // real accelerator -- so pasting the literal here would put "Ctrl+I" in a
    // tooltip for a key that is actually Cmd-I. ToString() renders whatever the
    // platform would draw in a menu, symbols and all.
    //
    // An entry wx cannot parse falls back to the literal: a slightly wrong
    // tooltip beats a missing one, and the menu bar would be just as wrong.
    wxString shortcut = wxString::FromUTF8(accelerator.data(), accelerator.size());
    if (const std::unique_ptr<wxAcceleratorEntry> entry{
            wxAcceleratorEntry::Create("\t" + shortcut)};
        entry && entry->IsOk()) {
        shortcut = entry->ToString();
    }

    return label + " (" + shortcut + ")";
}

wxMenuBar* buildMenuBar() {
    auto*    bar     = new wxMenuBar;
    wxMenu*  current = nullptr;
    wxString title;

    const auto flush = [&] {
        if (current != nullptr) {
            bar->Append(current, title);
        }
    };

    for (const MenuItem& item : menuLayout()) {
        if (item.menu != nullptr) {
            flush();
            current = new wxMenu;
            // Translated here rather than in the table: wxTRANSLATE only marks
            // a literal, so the catalogue is consulted at the moment the menu is
            // built -- which is what lets the same table be read for a tray menu
            // or a context menu without each of them remembering to translate.
            title   = trUtf8(item.menu);
        }
        if (current == nullptr) {
            continue;
        }
        appendItem(*current, item);
    }
    flush();

    return bar;
}

wxMenu* buildMenu(const std::vector<MenuItem>& items) {
    auto* menu = new wxMenu;
    for (const MenuItem& item : items) {
        appendItem(*menu, item);
    }
    return menu;
}

}  // namespace xpcog::app
