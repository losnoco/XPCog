// Preferences: the counterpart of app/src/PreferencesDialog.hpp, with one
// page per wx pane.
//
// Not an AdwPreferencesDialog. That dialog switches pages with a view
// switcher across its header, which is libadwaita's idiom for a handful of
// pages; twelve crowd it into icons without labels. This is the layout GNOME
// Settings uses instead: an AdwNavigationSplitView with an
// AdwViewSwitcherSidebar down the left, the pages grouped into sections, and
// the sidebar becoming a page of its own when the dialog is too narrow for
// both. Search comes back as a filter on the sidebar -- a page stays listed
// while any row on it matches -- rather than AdwPreferencesDialog's list of
// matching rows.
//
// The rows are built in code from a small vocabulary -- toggle, choice,
// number, text, path, colour, note, link -- over AdwPreferencesGroups, the
// way the wx dialog's RowBuilder builds them over a form, and every row does
// the same two things on a change: writes the setting, and publishes the key
// so the window can hand it to the session. The wording is the wx dialog's,
// under the same msgids, so one catalogue covers both.

#pragma once

#include "Glib.hpp"
#include "Session.hpp"

#include "xpcog/core/Signal.hpp"

#include <adwaita.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xpcog::gtk {

/// Which page the dialog opens on. Only the ones something asks for by name.
enum class PreferencesPage { Playlist, PitchTempo, Visualizers };

class PreferencesDialog {
public:
    /// `hasTray` is whether the window has a tray icon to close to: asked of
    /// the window rather than probed here, because a probe is a second
    /// StatusNotifierItem in the same process, and it owns the same bus
    /// name as the real one and takes it down with it when it goes.
    PreferencesDialog(app::Session& session, bool hasTray);
    ~PreferencesDialog();

    PreferencesDialog(const PreferencesDialog&)            = delete;
    PreferencesDialog& operator=(const PreferencesDialog&) = delete;

    /// Presents the dialog over `parent`, on `page`. The dialog owns itself
    /// from here: it is destroyed when closed, and this object with it -- see
    /// the .cpp for how.
    void present(GtkWidget* parent, PreferencesPage page);

    /// A setting was written by one of the rows. Carries the key.
    Signal<std::string> settingChanged;

    /// The dialog was closed; the owner may drop this object afterwards.
    Signal<> closed;

private:
    class RowBuilder;

    void buildPlaylistPage();
    void buildOutputPage();
    void buildPitchTempoPage();
    void buildGeneralPage();
    void buildNotificationsPage();
    void buildLastFmPage();
    void buildListenBrainzPage();
    void buildAppearancePage();
    void buildMidiPage();
    void buildVisualizersPage();
    void buildRemotePage();
    void buildAdvancedPage();

    /// The next page added starts a section in the sidebar, titled `title`
    /// or, when empty, set off by a separator alone.
    void startSection(std::string title) { pendingSection_ = std::move(title); }

    /// Everything a reader could search for on each page, keyed by the
    /// page's title, which is what the sidebar's items carry. Read once, after
    /// the pages are built.
    void indexPages();
    [[nodiscard]] bool pageMatches(const char* title) const;

    [[nodiscard]] AdwPreferencesPage* addPage(const char* name, const std::string& title,
                                              const char* icon);
    /// A page and the builder for its rows, kept alive with the dialog.
    [[nodiscard]] RowBuilder* rowsFor(const char* name, const std::string& title, const char* icon);

    app::Session& session_;
    Settings&     settings_;
    bool          hasTray_;

    GObjectPtr<AdwDialog>   dialog_;
    AdwNavigationSplitView* split_       = nullptr;
    AdwNavigationPage*      contentPage_ = nullptr;
    AdwViewStack*           stack_       = nullptr;
    GtkWidget*              search_      = nullptr;
    GtkCustomFilter*        filter_      = nullptr;  ///< Owned by the sidebar.

    std::optional<std::string>         pendingSection_;
    std::map<std::string, std::string> pageText_;

    /// Everything the pages keep alive: builders, closures' state.
    std::vector<std::shared_ptr<void>> keep_;
    std::vector<Connection>            connections_;

    /// So a handler dispatched after the dialog has gone finds nothing.
    std::shared_ptr<int> alive_;
};

}  // namespace xpcog::gtk
