#include "CommandMenus.hpp"

#include "Translations.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace xpcog::winui {

namespace {

using app::CommandId;
using app::ItemKind;
using winrt::Windows::System::VirtualKey;
using winrt::Windows::System::VirtualKeyModifiers;

struct Label {
    std::string text;
    char        accessKey = 0;  ///< the mnemonic letter, 0 when there is none
};

/// The translated label with its mnemonic taken out and kept. WinUI has no
/// `&` convention; what it has is AccessKey, which Alt shows as a key tip --
/// the same letter, reached the Windows 11 way.
Label label(const char* msgid) {
    const std::string translated = app::tr(msgid);
    Label out;
    for (std::size_t i = 0; i < translated.size(); ++i) {
        if (translated[i] != '&') {
            out.text.push_back(translated[i]);
            continue;
        }
        if (i + 1 < translated.size() && translated[i + 1] == '&') {
            out.text.push_back('&');
            ++i;
        } else if (i + 1 < translated.size() && out.accessKey == 0 &&
                   static_cast<unsigned char>(translated[i + 1]) < 0x80) {
            out.accessKey = static_cast<char>(std::toupper(static_cast<unsigned char>(translated[i + 1])));
        }
    }
    return out;
}

struct Shortcut {
    VirtualKey          key       = VirtualKey::None;
    VirtualKeyModifiers modifiers = VirtualKeyModifiers::None;
};

/// uicore's shortcut spelling -- "Ctrl+Shift+O", "Del", "Ctrl+," -- as a
/// WinUI key and modifiers. Every spelling the table uses is covered; one it
/// does not know comes back as VirtualKey::None and is shown but not bound.
Shortcut parseShortcut(std::string_view text) {
    Shortcut out;
    while (!text.empty()) {
        const std::size_t plus = text.find('+', 1);  // from 1, so "Ctrl++" could name '+'
        const std::string_view part = text.substr(0, plus);
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);

        if (part == "Ctrl") {
            out.modifiers = out.modifiers | VirtualKeyModifiers::Control;
        } else if (part == "Shift") {
            out.modifiers = out.modifiers | VirtualKeyModifiers::Shift;
        } else if (part == "Alt") {
            out.modifiers = out.modifiers | VirtualKeyModifiers::Menu;
        } else if (part.size() == 1 && std::isalnum(static_cast<unsigned char>(part[0]))) {
            out.key = static_cast<VirtualKey>(std::toupper(static_cast<unsigned char>(part[0])));
        } else if (part == ",") {
            out.key = static_cast<VirtualKey>(0xBC);  // VK_OEM_COMMA
        } else if (part == ".") {
            out.key = static_cast<VirtualKey>(0xBE);  // VK_OEM_PERIOD
        } else if (part == "Del" || part == "Delete") {
            out.key = VirtualKey::Delete;
        } else if (part == "Left") {
            out.key = VirtualKey::Left;
        } else if (part == "Right") {
            out.key = VirtualKey::Right;
        } else if (part == "Up") {
            out.key = VirtualKey::Up;
        } else if (part == "Down") {
            out.key = VirtualKey::Down;
        } else if (part == "Space") {
            out.key = VirtualKey::Space;
        } else if (part == "Enter") {
            out.key = VirtualKey::Enter;
        } else if (part.size() >= 2 && part[0] == 'F') {
            const int n = std::atoi(std::string(part.substr(1)).c_str());
            if (n >= 1 && n <= 12) {
                out.key = static_cast<VirtualKey>(static_cast<int>(VirtualKey::F1) + n - 1);
            }
        }
    }
    return out;
}

/// Whether a text box has its own use for this key: the ones it edits or
/// moves the caret with.
bool textBoxWants(const Shortcut& shortcut) {
    const bool ctrl = shortcut.modifiers == VirtualKeyModifiers::Control;
    const bool none = shortcut.modifiers == VirtualKeyModifiers::None;
    switch (shortcut.key) {
        case VirtualKey::Delete:
            return none;
        case VirtualKey::A:
        case VirtualKey::Z:
        case VirtualKey::Y:
        case VirtualKey::Left:
        case VirtualKey::Right:
            return ctrl;
        default:
            return false;
    }
}

}  // namespace

CommandMenus::CommandMenus(Hooks hooks) : hooks_(std::move(hooks)) {
    menuBar_ = mux::Controls::MenuBar();

    const std::vector<app::MenuItem>& rows = app::menuLayout();
    std::size_t start = 0;
    for (std::size_t i = 1; i <= rows.size(); ++i) {
        if (i < rows.size() && rows[i].menu == nullptr) {
            continue;
        }
        // rows[start, i) is one menu, titled by its first row.
        const Label title = label(rows[start].menu);
        auto        menu  = mux::Controls::MenuBarItem();
        menu.Title(toH(title.text));
        if (title.accessKey != 0) {
            menu.AccessKey(winrt::hstring(std::wstring(1, static_cast<wchar_t>(title.accessKey))));
        }
        fill(menu.Items(), rows, start, i, /*withAccelerators=*/true);
        if (menu.Items().Size() > 0) {
            menuBar_.Items().Append(menu);
        }
        start = i;
    }
    refresh();
}

void CommandMenus::attachAccelerators(const mux::UIElement& target) {
    for (const auto& accelerator : accelerators_) {
        target.KeyboardAccelerators().Append(accelerator);
    }
}

void CommandMenus::fill(
    const winrt::Windows::Foundation::Collections::IVector<mux::Controls::MenuFlyoutItemBase>& items,
    const std::vector<app::MenuItem>& rows, std::size_t first, std::size_t last,
    bool withAccelerators) {
    std::wstring radioGroup;
    bool         pendingSeparator = false;
    for (std::size_t i = first; i < last; ++i) {
        const app::MenuItem& row = rows[i];
        pendingSeparator = pendingSeparator || row.separatorBefore;
        if (hooks_.offered && !hooks_.offered(row.id)) {
            continue;
        }
        // A separator only between two items that are both shown, so a left
        // out command cannot leave two rules together or one at an edge.
        if (pendingSeparator && items.Size() > 0) {
            items.Append(mux::Controls::MenuFlyoutSeparator());
        }
        pendingSeparator = false;

        // Consecutive radio rows are one group; anything else ends it, and so
        // does a separator -- Repeat and Shuffle are adjacent, divided only by
        // one, and as one group checking the one would clear the other.
        if (row.kind == ItemKind::Radio) {
            if (radioGroup.empty() || row.separatorBefore ||
                (i > first && rows[i - 1].kind != ItemKind::Radio)) {
                radioGroup = L"group" + std::to_wstring(static_cast<int>(row.id));
            }
        } else {
            radioGroup.clear();
        }
        items.Append(makeItem(row, withAccelerators, radioGroup));
    }
}

mux::Controls::MenuFlyoutItemBase CommandMenus::makeItem(const app::MenuItem& row,
                                                        bool withAccelerator,
                                                        const std::wstring& radioGroup) {
    const Label text = label(row.label);

    mux::Controls::MenuFlyoutItem item{nullptr};
    switch (row.kind) {
        case ItemKind::Check:
            item = mux::Controls::ToggleMenuFlyoutItem();
            break;
        case ItemKind::Radio: {
            auto radio = mux::Controls::RadioMenuFlyoutItem();
            radio.GroupName(winrt::hstring(radioGroup));
            item = radio;
            break;
        }
        case ItemKind::Normal:
            item = mux::Controls::MenuFlyoutItem();
            break;
    }
    item.Text(toH(text.text));
    if (text.accessKey != 0) {
        item.AccessKey(winrt::hstring(std::wstring(1, static_cast<wchar_t>(text.accessKey))));
    }

    const CommandId id = row.id;
    // Click, not the toggle's own state change: the setting the item stands for
    // is the truth, and refresh() puts the tick where the setting says.
    item.Click([this, id](auto&&, auto&&) {
        if (hooks_.enabled(id)) {
            hooks_.run(id);
        }
        refresh();
    });

    if (row.accelerator != nullptr && *row.accelerator != '\0') {
        const Shortcut shortcut = parseShortcut(row.accelerator);
        if (withAccelerator && shortcut.key != VirtualKey::None) {
            // Not on the item: the items of a closed menu are not in the
            // window's tree, where an accelerator has to be to hear a key. So
            // it is made here and attached to the window (attachAccelerators),
            // and the item only shows its text.
            auto accelerator = mux::Input::KeyboardAccelerator();
            accelerator.Key(shortcut.key);
            accelerator.Modifiers(shortcut.modifiers);
            accelerator.Invoked([this, id](auto&&, mux::Input::KeyboardAcceleratorInvokedEventArgs const& args) {
                args.Handled(true);
                if (hooks_.enabled(id)) {
                    hooks_.run(id);
                }
                refresh();
            });
            accelerators_.push_back(accelerator);
            if (textBoxWants(shortcut)) {
                accelerator.IsEnabled(!inText_);
                textKeys_.push_back(accelerator);
            }
        }
        // The key shown beside the item, in the menu and the context menu
        // alike.
        std::string shown = row.accelerator;
        if (shown == "Del") {
            shown = "Delete";
        }
        item.KeyboardAcceleratorTextOverride(toH(shown));
    }

    if (withAccelerator) {
        items_.emplace(id, item);
    }
    apply(item, id);
    return item;
}

mux::Controls::MenuFlyout CommandMenus::playlistMenu() {
    auto flyout = mux::Controls::MenuFlyout();
    const std::vector<app::MenuItem>& rows = app::playlistMenuLayout();
    fill(flyout.Items(), rows, 0, rows.size(), /*withAccelerators=*/false);
    return flyout;
}

void CommandMenus::apply(const mux::Controls::MenuFlyoutItemBase& item, CommandId id) const {
    item.IsEnabled(hooks_.enabled(id));
    if (const std::optional<bool> on = hooks_.checked(id)) {
        if (const auto toggle = item.try_as<mux::Controls::ToggleMenuFlyoutItem>()) {
            toggle.IsChecked(*on);
        } else if (const auto radio = item.try_as<mux::Controls::RadioMenuFlyoutItem>()) {
            radio.IsChecked(*on);
        }
    }
}

void CommandMenus::refresh() {
    for (const auto& [id, item] : items_) {
        apply(item, id);
    }
}

void CommandMenus::setTextFocus(bool inText) {
    if (inText == inText_) {
        return;
    }
    inText_ = inText;
    for (const auto& accelerator : textKeys_) {
        accelerator.IsEnabled(!inText);
    }
}

}  // namespace xpcog::winui
