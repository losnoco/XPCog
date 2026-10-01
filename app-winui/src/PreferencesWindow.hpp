#pragma once

#include "WinRT.hpp"

#include "xpcog/core/Signal.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {
class Settings;
}
namespace xpcog::app {
class Session;
}

namespace xpcog::winui {

/// Where Preferences opens when something other than the menu asks for it.
enum class PreferencesPage { Playlist, PitchTempo, Visualizers };

/// Preferences: a window of its own, laid out like Windows 11's Settings --
/// Mica, a NavigationView of pages down the left with a search box over it,
/// and each page a column of setting cards under group headings.
///
/// The pages are the GTK dialog's (app-gtk/src/PreferencesDialog.cpp), in the
/// same order and sections, with the same rows, keys and wording, built
/// through a RowBuilder (PreferencesRows.hpp) with the same vocabulary --
/// toggle, choice, picker, number, text, path, colour, note, link -- so a page
/// reads the same in both and a change to one shows where the other needs it.
class PreferencesWindow {
public:
    /// `hasTray`: whether the player has a tray icon to close to, which is
    /// what decides whether Close to tray is offered.
    PreferencesWindow(app::Session& session, HWND owner, bool hasTray);
    ~PreferencesWindow();

    PreferencesWindow(const PreferencesWindow&)            = delete;
    PreferencesWindow& operator=(const PreferencesWindow&) = delete;

    /// Shows the window at `page`, or brings it forward at that page.
    void show(std::optional<PreferencesPage> page = std::nullopt);
    void close();

    /// A setting was written: the session decides what it affects.
    Signal<std::string> settingChanged;
    /// The window has closed; it may be destroyed after this returns.
    Signal<> closed;

    class RowBuilder;

private:
    // --- the pages, each in the file its group is written in -------------------
    // PreferencesGeneral.cpp
    void buildGeneralPage();
    void buildPlaylistPage();
    void buildAppearancePage();
    void buildNotificationsPage();
    void buildVisualizersPage();
    // PreferencesSound.cpp
    void buildOutputPage();
    void buildPitchTempoPage();
    void buildMidiPage();
    // PreferencesServices.cpp
    void buildLastFmPage();
    void buildListenBrainzPage();
    void buildRemotePage();
    void buildAdvancedPage();

    /// The next page starts a section of the navigation, under `title` (none
    /// for a bare rule).
    void startSection(std::string title) { pendingSection_ = std::move(title); }
    /// A page: its navigation item, its scrolling column, and the rows that
    /// fill it. `glyph` is a Segoe Fluent Icons code point.
    [[nodiscard]] RowBuilder* rowsFor(const char* name, const std::string& title, const wchar_t* glyph);

    void select(const std::string& name);
    void filter(const std::string& query);

    app::Session& session_;
    Settings&     settings_;
    /// Whether the player has a tray icon for close-to-tray to send it to.
    bool hasTray_ = false;

    mux::Window                         window_{nullptr};
    mux::Controls::NavigationView       navigation_{nullptr};
    mux::Controls::Grid                 content_{nullptr};
    std::optional<std::string>          pendingSection_;

    struct Page {
        std::string                         name;
        mux::Controls::NavigationViewItem   item{nullptr};
        mux::FrameworkElement               view{nullptr};
        /// Every word on the page, folded, for the search.
        std::string                         text;
        /// The header this page sits under, if it starts or continues one.
        mux::Controls::NavigationViewItemHeader header{nullptr};
    };
    std::vector<Page>                        pages_;
    std::vector<std::shared_ptr<RowBuilder>> builders_;
    /// Whatever a page's rows need kept alive with the window.
    std::vector<std::shared_ptr<void>>       keep_;
    std::vector<Subscription>                subscriptions_;
    std::shared_ptr<int>                     alive_;
    bool                                     closing_ = false;
};

}  // namespace xpcog::winui
