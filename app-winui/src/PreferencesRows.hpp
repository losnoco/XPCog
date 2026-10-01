#pragma once

// The row vocabulary the Preferences pages are written in: GTK's RowBuilder
// (app-gtk/src/PreferencesDialog.cpp) for WinUI. Only the page files and
// PreferencesWindow.cpp include this.
//
// Each row is a Windows 11 settings card -- title and description on the
// left, the control on the right, on the card fill -- and comes back as a
// Row, which the page can hide, show or disable when another setting changes
// what applies.

#include "PreferencesWindow.hpp"

#include "SettingChoices.hpp"
#include "Translations.hpp"

#include "xpcog/core/Settings.hpp"

#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::winui {

/// A row on a page.
struct Row {
    /// The whole card, or the note or the button strip.
    mux::FrameworkElement element{nullptr};
    /// What the row edits -- the toggle, the combo box, the number box -- or
    /// null for a note. Reach for the specific type with .as<>().
    mux::Controls::Control control{nullptr};

    void show(bool shown) const {
        if (element) {
            element.Visibility(shown ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        }
    }
    /// Disabled greys the control and dims the row's text with it, which a
    /// Control's IsEnabled alone does not reach.
    void enable(bool enabled) const;
};

class PreferencesWindow::RowBuilder {
public:
    using Announce = std::function<void(const char*)>;

    RowBuilder(Settings& settings, mux::Controls::StackPanel column, Announce announce,
               std::function<winrt::Microsoft::UI::WindowId()> windowId, std::string& searchText);

    /// A group heading; the rows after it belong to it.
    void heading(const std::string& title);

    Row toggle(const std::string& label, const char* key, const std::string& hint = {});

    /// A fixed list of values for `key`, labelled by the msgids in `choices`.
    Row choice(const std::string& label, const char* key, std::span<const app::Choice> choices,
               std::function<void(const std::string&)> onChange = {});

    /// A list the page builds itself: names, which one is chosen, and what
    /// to do when another is. The page writes the setting.
    Row picker(const std::string& label, const std::vector<std::string>& names, uint32_t selected,
               std::function<void(uint32_t)> onChange);

    Row number(const std::string& label, const char* key, double minimum, double maximum,
               double step = 1.0, unsigned digits = 0);

    /// Written when the box is left or Enter is pressed, not per keystroke.
    Row text(const std::string& label, const char* key, bool secret = false);

    /// A path, shown as the row's description, with buttons opening a folder
    /// picker, a file picker, or both. `patterns` filter the file picker:
    /// "*.sf2" and the like.
    Row path(const std::string& label, const char* key, bool folders, bool files,
             std::vector<std::string> patterns);

    /// A swatch that opens a colour picker; written as "#rrggbb".
    Row colour(const std::string& label, const char* key, const char* fallback);

    /// A paragraph under the rows so far. `quiet` is the secondary text a
    /// note usually is; loud is primary and selectable.
    Row note(const std::string& text, bool quiet = true);

    /// A row that opens `url` in the browser.
    Row link(const std::string& label, std::string_view url);

    /// A row with anything on its right: a control, which becomes the row's
    /// Row::control, or a panel of several -- a slider and its readout --
    /// where Row::control is null and the page keeps what it needs.
    Row add(const std::string& label, const mux::FrameworkElement& content,
            const std::string& hint = {});

    /// A row of buttons, under the rows so far.
    Row buttons(std::initializer_list<mux::Controls::Button> buttons);

    Settings&       settings() { return settings_; }
    const Announce& announce() const { return announce_; }

private:
    Row card(const std::string& label, const std::string& hint, const mux::FrameworkElement& right,
             const mux::Controls::Control& control);
    void remember(std::string_view text);

    Settings&                                       settings_;
    mux::Controls::StackPanel                       column_;
    Announce                                        announce_;
    std::function<winrt::Microsoft::UI::WindowId()> windowId_;
    std::string&                                    searchText_;
};

/// Opens a page's builder: `row` is what the page writes its rows through,
/// as GTK's XPCOG_ROWS has it.
#define XPCOG_ROWS(name, title, glyph) RowBuilder* row = rowsFor(name, title, glyph)

}  // namespace xpcog::winui
