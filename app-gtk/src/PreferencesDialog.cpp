#include "PreferencesDialog.hpp"

#include "LastFmAccount.hpp"
#include "ListenBrainzAccount.hpp"
#include "RemoteToken.hpp"
#include "SettingChoices.hpp"
#include "SpeedCurve.hpp"
#include "Translations.hpp"

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/remote/RemoteServer.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/OpenUrl.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <span>
#include <string_view>

namespace xpcog::gtk {

using app::Choice;
using app::tr;

namespace {

[[nodiscard]] bool isTrue(const std::string& text) {
    // Cog's plist stores YES/NO; Settings accepts both those and true/false.
    return text == "1" || text == "true" || text == "YES";
}

[[nodiscard]] int toInt(const std::string& text) {
    try {
        return text.empty() ? 0 : std::stoi(text);
    } catch (const std::exception&) {
        return 0;
    }
}

[[nodiscard]] double toDouble(const std::string& text) {
    try {
        return text.empty() ? 0.0 : std::stod(text);
    } catch (const std::exception&) {
        return 0.0;
    }
}

/// "#rrggbb", lower case, from a GdkRGBA -- the spelling the settings hold
/// and the wx picker writes.
[[nodiscard]] std::string hexOf(const GdkRGBA& rgba) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
                  static_cast<int>(std::lround(static_cast<double>(rgba.red) * 255.0)),
                  static_cast<int>(std::lround(static_cast<double>(rgba.green) * 255.0)),
                  static_cast<int>(std::lround(static_cast<double>(rgba.blue) * 255.0)));
    return buffer;
}

[[nodiscard]] std::string trimmed(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    const auto end   = text.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, end - begin + 1);
}

}  // namespace

// --- the row vocabulary ------------------------------------------------------------

class PreferencesDialog::RowBuilder {
public:
    using Announce = std::function<void(const char*)>;

    RowBuilder(Settings& settings, AdwPreferencesPage* page, Announce announce,
               std::vector<Connection>& connections)
        : settings_(settings), page_(page), announce_(std::move(announce)),
          connections_(connections) {}

    /// Starts a titled group; rows go into it until the next heading.
    void heading(const std::string& title) {
        group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(group_, title.c_str());
        adw_preferences_page_add(page_, group_);
        groupHasNote_ = false;
    }

    GtkWidget* toggle(const std::string& label, const char* key, const std::string& hint = {}) {
        GtkWidget* row = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        if (!hint.empty()) {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), hint.c_str());
        }
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), isTrue(settings_.rawValue(key)));
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            row, "notify::active", [this, key, row](GObject*, GParamSpec*) {
                settings_.setRawValue(key, adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? "true"
                                                                                           : "false");
                announce_(key);
            }));
        add(row);
        return row;
    }

    GtkWidget* choice(const std::string& label, const char* key, std::span<const Choice> choices,
                      std::function<void(const std::string&)> onChange = {}) {
        auto names = GObjectPtr<GtkStringList>::adopt(gtk_string_list_new(nullptr));
        std::vector<std::string> values;
        for (const Choice& option : choices) {
            gtk_string_list_append(names.get(), tr(option.label).c_str());
            values.emplace_back(option.value);
        }
        GtkWidget* row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(names.get()));
        const std::string current = settings_.rawValue(key);
        guint             index   = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (current == values[i]) {
                index = static_cast<guint>(i);
                break;
            }
        }
        adw_combo_row_set_selected(ADW_COMBO_ROW(row), index);
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            row, "notify::selected",
            [this, key, row, values, onChange = std::move(onChange)](GObject*, GParamSpec*) {
                const guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
                if (selected < values.size()) {
                    settings_.setRawValue(key, values[selected]);
                    announce_(key);
                    if (onChange) {
                        onChange(values[selected]);
                    }
                }
            }));
        add(row);
        return row;
    }

    /// A picker over names the caller supplies, with its own commit.
    GtkWidget* picker(const std::string& label, const std::vector<std::string>& names,
                      guint selected, std::function<void(guint)> onChange) {
        auto list = GObjectPtr<GtkStringList>::adopt(gtk_string_list_new(nullptr));
        for (const std::string& name : names) {
            gtk_string_list_append(list.get(), name.c_str());
        }
        GtkWidget* row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list.get()));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            row, "notify::selected", [row, onChange = std::move(onChange)](GObject*, GParamSpec*) {
                onChange(adw_combo_row_get_selected(ADW_COMBO_ROW(row)));
            }));
        add(row);
        return row;
    }

    GtkWidget* number(const std::string& label, const char* key, double minimum, double maximum,
                      double step = 1.0, guint digits = 0) {
        GtkWidget* row = adw_spin_row_new_with_range(minimum, maximum, step);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_spin_row_set_digits(ADW_SPIN_ROW(row), digits);
        adw_spin_row_set_value(ADW_SPIN_ROW(row), toDouble(settings_.rawValue(key)));
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            row, "notify::value", [this, key, row, digits](GObject*, GParamSpec*) {
                const double value = adw_spin_row_get_value(ADW_SPIN_ROW(row));
                settings_.setRawValue(key, digits == 0
                                               ? std::to_string(static_cast<long long>(std::lround(value)))
                                               : std::to_string(value));
                announce_(key);
            }));
        add(row);
        return row;
    }

    GtkWidget* text(const std::string& label, const char* key, bool secret = false) {
        GtkWidget* row = secret ? adw_password_entry_row_new() : adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        gtk_editable_set_text(GTK_EDITABLE(row), settings_.rawValue(key).c_str());
        adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
        connections_.push_back(Connection::to<void(AdwEntryRow*)>(
            row, "apply", [this, key](AdwEntryRow* entry) {
                settings_.setRawValue(key, gtk_editable_get_text(GTK_EDITABLE(entry)));
                announce_(key);
            }));
        add(row);
        return row;
    }

    /// A path in the subtitle, with buttons that open pickers for it.
    GtkWidget* path(const std::string& label, const char* key, bool folders, bool files,
                    std::vector<std::string> patterns) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), settings_.rawValue(key).c_str());
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);

        struct Chosen {
            Settings*   settings;
            Announce    announce;
            const char* key;
            GtkWidget*  row;
        };
        auto state = std::make_shared<Chosen>(Chosen{&settings_, announce_, key, row});
        keep_.push_back(state);

        const auto apply = [](Chosen& c, GFile* file) {
            GStr path(g_file_get_path(file));
            if (!path) {
                return;
            }
            c.settings->setRawValue(c.key, path.str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(c.row), path.c_str());
            c.announce(c.key);
        };

        if (folders) {
            GtkWidget* button = gtk_button_new_with_label(tr("Folder...").c_str());
            gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
            connections_.push_back(Connection::to<void(GtkButton*)>(
                button, "clicked", [state, label, apply](GtkButton* b) {
                    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
                    gtk_file_dialog_set_title(dialog.get(), label.c_str());
                    auto* keep = new std::pair<std::shared_ptr<Chosen>, decltype(apply)>(state, apply);
                    gtk_file_dialog_select_folder(
                        dialog.release(), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), nullptr,
                        [](GObject* source, GAsyncResult* result, gpointer data) {
                            std::unique_ptr<std::pair<std::shared_ptr<Chosen>, decltype(apply)>> p(
                                static_cast<std::pair<std::shared_ptr<Chosen>, decltype(apply)>*>(data));
                            auto owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
                            GErrorPtr error;
                            auto folder = GObjectPtr<GFile>::adopt(
                                gtk_file_dialog_select_folder_finish(owned.get(), result, &error.value));
                            if (folder) {
                                p->second(*p->first, folder.get());
                            }
                        },
                        keep);
                }));
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        }
        if (files) {
            GtkWidget* button = gtk_button_new_with_label(
                folders ? tr("Archive...").c_str() : tr("Choose...").c_str());
            gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
            connections_.push_back(Connection::to<void(GtkButton*)>(
                button, "clicked", [state, label, patterns, apply](GtkButton* b) {
                    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
                    gtk_file_dialog_set_title(dialog.get(), label.c_str());
                    if (!patterns.empty()) {
                        auto filters = GObjectPtr<GListStore>::adopt(g_list_store_new(GTK_TYPE_FILE_FILTER));
                        auto filter  = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
                        for (const std::string& pattern : patterns) {
                            gtk_file_filter_add_pattern(filter.get(), pattern.c_str());
                        }
                        gtk_file_filter_set_name(filter.get(), label.c_str());
                        auto all = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
                        gtk_file_filter_add_pattern(all.get(), "*");
                        gtk_file_filter_set_name(all.get(), tr("All Files").c_str());
                        g_list_store_append(filters.get(), filter.get());
                        g_list_store_append(filters.get(), all.get());
                        gtk_file_dialog_set_filters(dialog.get(), G_LIST_MODEL(filters.get()));
                        gtk_file_dialog_set_default_filter(dialog.get(), filter.get());
                    }
                    auto* keep = new std::pair<std::shared_ptr<Chosen>, decltype(apply)>(state, apply);
                    gtk_file_dialog_open(
                        dialog.release(), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b))), nullptr,
                        [](GObject* source, GAsyncResult* result, gpointer data) {
                            std::unique_ptr<std::pair<std::shared_ptr<Chosen>, decltype(apply)>> p(
                                static_cast<std::pair<std::shared_ptr<Chosen>, decltype(apply)>*>(data));
                            auto owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
                            GErrorPtr error;
                            auto file = GObjectPtr<GFile>::adopt(
                                gtk_file_dialog_open_finish(owned.get(), result, &error.value));
                            if (file) {
                                p->second(*p->first, file.get());
                            }
                        },
                        keep);
                }));
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        }
        add(row);
        return row;
    }

    GtkWidget* colour(const std::string& label, const char* key, const char* fallback) {
        GdkRGBA rgba;
        if (!gdk_rgba_parse(&rgba, settings_.rawValue(key).c_str())) {
            gdk_rgba_parse(&rgba, fallback);
        }
        GtkWidget* button = gtk_color_dialog_button_new(gtk_color_dialog_new());
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            button, "notify::rgba", [this, key, button](GObject*, GParamSpec*) {
                settings_.setRawValue(
                    key, hexOf(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button))));
                announce_(key);
            }));
        return add(label, button);
    }

    /// A paragraph under the rows so far. Ends the group, so the next row
    /// starts a new one and the paragraph stays where it was written.
    GtkWidget* note(const std::string& text, bool quiet = true) {
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_label_set_selectable(GTK_LABEL(label), !quiet);
        if (quiet) {
            gtk_widget_add_css_class(label, "dim-label");
        }
        gtk_widget_add_css_class(label, "caption");
        gtk_widget_set_margin_top(label, 6);
        gtk_widget_set_margin_start(label, 6);
        ensureGroup();
        adw_preferences_group_add(group_, label);
        groupHasNote_ = true;
        return label;
    }

    GtkWidget* link(const std::string& label, std::string_view url) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), nullptr);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                  gtk_image_new_from_icon_name("external-link-symbolic"));
        const std::string target(url);
        connections_.push_back(Connection::to<void(AdwActionRow*)>(
            row, "activated", [target](AdwActionRow*) { platform::openInBrowser(target); }));
        add(row);
        return row;
    }

    /// A row with any widget on its right, and an optional hint.
    GtkWidget* add(const std::string& label, GtkWidget* control, const std::string& hint = {}) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        if (!hint.empty()) {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), hint.c_str());
        }
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), control);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), control);
        add(row);
        return row;
    }

    /// A row of buttons, under the rows so far.
    GtkWidget* buttons(std::initializer_list<GtkWidget*> widgets) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(box, 6);
        for (GtkWidget* widget : widgets) {
            gtk_box_append(GTK_BOX(box), widget);
        }
        ensureGroup();
        adw_preferences_group_add(group_, box);
        groupHasNote_ = true;
        return box;
    }

    void add(GtkWidget* row) {
        // A row after a note or a button box starts a new group, so the order
        // the page was written in is the order it reads in.
        if (group_ == nullptr || groupHasNote_) {
            group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_page_add(page_, group_);
            groupHasNote_ = false;
        }
        adw_preferences_group_add(group_, row);
    }

    Settings& settings() { return settings_; }
    const Announce& announce() const { return announce_; }

private:
    void ensureGroup() {
        if (group_ == nullptr) {
            group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_page_add(page_, group_);
        }
    }

    Settings&                          settings_;
    AdwPreferencesPage*                page_;
    Announce                           announce_;
    std::vector<Connection>&           connections_;
    AdwPreferencesGroup*               group_        = nullptr;
    bool                               groupHasNote_ = false;
    std::vector<std::shared_ptr<void>> keep_;
};

// --- the dialog ----------------------------------------------------------------------

PreferencesDialog::PreferencesDialog(app::Session& session, bool hasTray)
    : session_(session),
      settings_(session.settings()),
      hasTray_(hasTray),
      alive_(std::make_shared<int>(0)) {
    dialog_ = GObjectPtr<AdwDialog>::sink(adw_dialog_new());
    adw_dialog_set_title(dialog_.get(), tr("Preferences").c_str());
    adw_dialog_set_content_width(dialog_.get(), 860);
    adw_dialog_set_content_height(dialog_.get(), 640);
    // A dialog with breakpoints has to say how small it may go, or libadwaita
    // cannot tell which of them apply; AdwPreferencesDialog's own floor.
    gtk_widget_set_size_request(GTK_WIDGET(dialog_.get()), 360, 294);
    connections_.push_back(Connection::to<void(AdwDialog*)>(
        dialog_.get(), "closed", [this](AdwDialog*) { closed.publish(); }));

    stack_ = ADW_VIEW_STACK(adw_view_stack_new());

    // The sidebar pane: a search entry over the page list.
    GtkWidget* sidebar = adw_view_switcher_sidebar_new();
    adw_view_switcher_sidebar_set_stack(ADW_VIEW_SWITCHER_SIDEBAR(sidebar), stack_);
    filter_ = gtk_custom_filter_new(
        [](gpointer item, gpointer self) -> gboolean {
            g_autofree char* title = nullptr;
            g_object_get(item, "title", &title, nullptr);
            return static_cast<PreferencesDialog*>(self)->pageMatches(title);
        },
        this, nullptr);
    // The sidebar takes its own reference; ours goes with it.
    adw_view_switcher_sidebar_set_filter(ADW_VIEW_SWITCHER_SIDEBAR(sidebar), GTK_FILTER(filter_));
    g_object_unref(filter_);
    GtkWidget* nothing = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(nothing), "edit-find-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(nothing), tr("No Results Found").c_str());
    gtk_widget_add_css_class(nothing, "compact");
    adw_view_switcher_sidebar_set_placeholder(ADW_VIEW_SWITCHER_SIDEBAR(sidebar), nothing);

    search_ = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_), tr("Search").c_str());
    gtk_widget_set_margin_start(search_, 12);
    gtk_widget_set_margin_end(search_, 12);
    gtk_widget_set_margin_bottom(search_, 6);
    connections_.push_back(Connection::to<void(GtkSearchEntry*)>(
        search_, "search-changed", [this](GtkSearchEntry*) {
            gtk_filter_changed(GTK_FILTER(filter_), GTK_FILTER_CHANGE_DIFFERENT);
        }));

    GtkWidget* sidebarView = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebarView), adw_header_bar_new());
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebarView), search_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebarView), sidebar);
    AdwNavigationPage* sidebarPage =
        adw_navigation_page_new_with_tag(sidebarView, tr("Preferences").c_str(), "sidebar");

    // The content pane: the page, under a header titled with its name.
    GtkWidget* contentView = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(contentView), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(contentView), GTK_WIDGET(stack_));
    contentPage_ = adw_navigation_page_new_with_tag(contentView, "", "content");

    split_ = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_sidebar(split_, sidebarPage);
    adw_navigation_split_view_set_content(split_, contentPage_);
    adw_navigation_split_view_set_min_sidebar_width(split_, 200);
    adw_dialog_set_child(dialog_.get(), GTK_WIDGET(split_));

    // Too narrow for both panes, the sidebar becomes a page of boxed rows
    // and choosing one pushes the page over it -- libadwaita's own adaptive
    // pattern, and the one a phone-sized window needs.
    AdwBreakpoint* narrow = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 540sp"));
    adw_breakpoint_add_setters(narrow, G_OBJECT(split_), "collapsed", TRUE, G_OBJECT(sidebar),
                               "mode", ADW_SIDEBAR_MODE_PAGE, nullptr);
    adw_dialog_add_breakpoint(dialog_.get(), narrow);

    // Choosing a page names the content pane after it and, collapsed, shows it.
    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        stack_, "notify::visible-child", [this](GObject*, GParamSpec*) {
            GtkWidget* child = adw_view_stack_get_visible_child(stack_);
            if (child == nullptr) {
                return;
            }
            AdwViewStackPage* page = adw_view_stack_get_page(stack_, child);
            adw_navigation_page_set_title(contentPage_, adw_view_stack_page_get_title(page));
            adw_navigation_split_view_set_show_content(split_, TRUE);
        }));

    // Grouped the way a reader looks for things rather than in the wx
    // dialog's order, which is one long row of tabs: what the player does,
    // then how it sounds, then the services it talks to, and Advanced --
    // every key, raw -- set apart at the end.
    buildGeneralPage();
    buildPlaylistPage();
    buildAppearancePage();
    buildNotificationsPage();
    buildVisualizersPage();
    startSection(tr("Sound"));
    buildOutputPage();
    buildPitchTempoPage();
    buildMidiPage();
    startSection(tr("Services"));
    if (session_.lastFm() != nullptr && session_.scrobbler() != nullptr) {
        buildLastFmPage();
    }
    if (session_.listenBrainz() != nullptr && session_.listenBrainzScrobbler() != nullptr) {
        buildListenBrainzPage();
    }
    buildRemotePage();
    startSection({});
    buildAdvancedPage();

    indexPages();
}

PreferencesDialog::~PreferencesDialog() {
    if (app::LastFmAccount* account = session_.lastFm()) {
        account->cancelConnect();
    }
}

void PreferencesDialog::present(GtkWidget* parent, PreferencesPage page) {
    const char* name = "playlist";
    switch (page) {
        case PreferencesPage::Playlist:
            break;
        case PreferencesPage::PitchTempo:
            name = "pitch-tempo";
            break;
        case PreferencesPage::Visualizers:
            name = "visualizers";
            break;
    }
    adw_view_stack_set_visible_child_name(stack_, name);
    adw_dialog_present(dialog_.get(), parent);
}

namespace {

/// Appends every piece of text a reader could search for under `widget`:
/// labels, and the titles and subtitles rows and groups draw themselves.
void collectText(GtkWidget* widget, std::string& out) {
    const auto add = [&out](const char* text) {
        if (text != nullptr && *text != '\0') {
            out += text;
            out += '\n';
        }
    };
    if (ADW_IS_PREFERENCES_ROW(widget)) {
        add(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)));
    }
    if (ADW_IS_ACTION_ROW(widget)) {
        add(adw_action_row_get_subtitle(ADW_ACTION_ROW(widget)));
    }
    if (ADW_IS_EXPANDER_ROW(widget)) {
        add(adw_expander_row_get_subtitle(ADW_EXPANDER_ROW(widget)));
    }
    if (ADW_IS_PREFERENCES_GROUP(widget)) {
        add(adw_preferences_group_get_title(ADW_PREFERENCES_GROUP(widget)));
        add(adw_preferences_group_get_description(ADW_PREFERENCES_GROUP(widget)));
    }
    if (GTK_IS_LABEL(widget)) {
        add(gtk_label_get_text(GTK_LABEL(widget)));
    }
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        collectText(child, out);
    }
}

[[nodiscard]] std::string folded(const char* text) {
    g_autofree char* normal = g_utf8_normalize(text, -1, G_NORMALIZE_ALL);
    g_autofree char* fold   = g_utf8_casefold(normal != nullptr ? normal : text, -1);
    return fold;
}

}  // namespace

void PreferencesDialog::indexPages() {
    // Built once: a row's words are the dialog's own and do not change with
    // what the row is set to. The page's title is part of what it matches.
    GListModel* pages = G_LIST_MODEL(adw_view_stack_get_pages(stack_));
    for (guint i = 0; i < g_list_model_get_n_items(pages); ++i) {
        auto* page  = static_cast<AdwViewStackPage*>(g_list_model_get_item(pages, i));
        std::string text = adw_view_stack_page_get_title(page);
        text += '\n';
        collectText(adw_view_stack_page_get_child(page), text);
        pageText_[adw_view_stack_page_get_title(page)] = folded(text.c_str());
        g_object_unref(page);
    }
    g_object_unref(pages);
}

bool PreferencesDialog::pageMatches(const char* title) const {
    // Asked once while the sidebar is being set up, before the entry exists.
    if (search_ == nullptr) {
        return true;
    }
    const char* query = gtk_editable_get_text(GTK_EDITABLE(search_));
    if (query == nullptr || *query == '\0' || title == nullptr) {
        return true;
    }
    const auto found = pageText_.find(title);
    return found != pageText_.end() && found->second.find(folded(query)) != std::string::npos;
}

AdwPreferencesPage* PreferencesDialog::addPage(const char* name, const std::string& title,
                                               const char* icon) {
    auto* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(page, name);
    adw_preferences_page_set_title(page, title.c_str());
    adw_preferences_page_set_icon_name(page, icon);
    AdwViewStackPage* entry = adw_view_stack_add_titled_with_icon(stack_, GTK_WIDGET(page), name,
                                                                  title.c_str(), icon);
    if (pendingSection_) {
        adw_view_stack_page_set_starts_section(entry, TRUE);
        if (!pendingSection_->empty()) {
            adw_view_stack_page_set_section_title(entry, pendingSection_->c_str());
        }
        pendingSection_.reset();
    }
    return page;
}

PreferencesDialog::RowBuilder* PreferencesDialog::rowsFor(const char* name, const std::string& title,
                                                          const char* icon) {
    auto builder = std::make_shared<RowBuilder>(
        settings_, addPage(name, title, icon),
        [this](const char* key) { settingChanged.publish(key); }, connections_);
    keep_.push_back(builder);
    return builder.get();
}

#define XPCOG_ROWS(name, title, icon) RowBuilder* row = rowsFor(name, title, icon)

// --- pages ---------------------------------------------------------------------------

void PreferencesDialog::buildPlaylistPage() {
    XPCOG_ROWS("playlist", tr("Playlist"), "view-list-symbolic");
    row->toggle(tr("Stop after every track"), "alwaysStopAfterCurrent");
    row->toggle(tr("Follow the playing track in the playlist"), "selectionFollowsPlayback",
                tr("Move the selection to each track as it starts."));
    row->toggle(tr("Keep playing while looking for the next playable track"),
                "keepPlayingWhileSkipping",
                tr("When Next or Previous lands on a track that will not open, carry on playing "
                   "the current one until a playable track is found."));
    row->toggle(tr("Resume playback on startup"), "resumePlaybackOnStartup",
                tr("Continue the last track from where it stopped. The track is selected either "
                   "way."));
    row->toggle(tr("Read cue sheets when adding folders"), "readCueSheetsInFolders");
    row->toggle(tr("Read playlists when adding folders"), "readPlaylistsInFolders");
    row->toggle(tr("Ignore AppleDouble files"), "skipAppleDoubleFiles",
                tr("The \"._\" files macOS leaves beside the audio. They keep the track's "
                   "extension but hold no audio."));
}

void PreferencesDialog::buildOutputPage() {
    XPCOG_ROWS("output", tr("Output"), "audio-speakers-symbolic");

    std::vector<std::string> names;
    std::vector<std::string> ids;
    names.push_back(tr("System default"));
    ids.emplace_back();
    const std::string chosenId = settings_.rawValue("outputDeviceId");
    for (const DeviceInfo& device : enumerateOutputDevices()) {
        names.push_back(device.isDefault ? app::trf("%s (current default)", device.name)
                                         : device.name);
        ids.push_back(device.id);
    }
    guint chosen = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == chosenId) {
            chosen = static_cast<guint>(i);
            break;
        }
    }
    if (chosen == 0 && !chosenId.empty()) {
        const std::string name = settings_.rawValue("outputDeviceName");
        names.push_back(app::trf("%s (not connected)", name.empty() ? chosenId : name));
        ids.push_back(chosenId);
        chosen = static_cast<guint>(ids.size() - 1);
    }
    row->picker(tr("Output device"), names, chosen, [this, ids, names](guint index) {
        if (index >= ids.size()) {
            return;
        }
        settings_.setRawValue("outputDeviceId", ids[index]);
        settings_.setRawValue("outputDeviceName", ids[index].empty() ? std::string{} : names[index]);
        settingChanged.publish("outputDeviceId");
    });
    row->toggle(tr("Play exclusively"), "exclusiveOutput",
                tr("Use the file's own rate and format instead of the system mixer's. Other "
                   "applications cannot play while this is active. Falls back to sharing if the "
                   "device is unavailable."));
    row->choice(tr("Volume scaling"), "volumeScaling", app::kVolumeScalingChoices);
    row->choice(tr("Resampler quality"), "resampling", app::kResamplingChoices);
    row->toggle(tr("Decode HDCD"), "enableHDCD",
                tr("Applies to 16-bit 44.1 kHz stereo only. Files without HDCD codes are "
                   "unaffected."));
    row->toggle(tr("Halve DSD volume"), "halveDSDVolume",
                tr("DSD is converted with a filter whose gain puts half modulation "
                   "\xE2\x80\x94 as loud as most SACDs go \xE2\x80\x94 at full "
                   "scale. Turn on if a loud SACD rip clips."));
    row->toggle(tr("Upmix stereo to surround"), "enableFSurround",
                tr("Uses FreeSurround. Takes effect when the device is next opened."));
    row->toggle(tr("Fade on seek and stop"), "enableFading");
    row->toggle(tr("Release the device while paused"), "suspendOutputOnPause",
                tr("Let other applications use the device while playback is paused. Turn off to "
                   "keep an exclusive device reserved."));
    row->note(tr("Volume scaling and resampler quality apply from the next track. Changing the "
                 "device moves playback across with a brief gap."));
}

void PreferencesDialog::buildPitchTempoPage() {
    XPCOG_ROWS("pitch-tempo", tr("Pitch & Tempo"), "media-playlist-shuffle-symbolic");

    struct Rows {
        GtkWidget *pitch = nullptr, *tempo = nullptr, *lock = nullptr, *reset = nullptr;
        GtkWidget *transients = nullptr, *detector = nullptr, *phase = nullptr, *window = nullptr,
                  *smoothing = nullptr, *formant = nullptr, *pitchMode = nullptr,
                  *channels = nullptr, *note = nullptr;
        GtkWidget *pitchScale = nullptr, *pitchValue = nullptr, *tempoScale = nullptr,
                  *tempoValue = nullptr;
        std::vector<std::string> windowValues;
        bool                     syncing = false;
    };
    auto rows = std::make_shared<Rows>();
    keep_.push_back(rows);

    // The window row is rebuilt for the engine: Finer has no long window.
    const auto rebuildWindow = [this, rows](bool finer) {
        std::string current = settings_.RubberbandWindow();
        if (finer && current == "long") {
            current = "standard";
            settings_.setRawValue("rubberbandWindow", current);
            settingChanged.publish("rubberbandWindow");
        }
        auto names = GObjectPtr<GtkStringList>::adopt(gtk_string_list_new(nullptr));
        rows->windowValues.clear();
        guint selected = 0;
        for (const Choice& option : app::kRubberWindowChoices) {
            if (finer && std::string_view{option.value} == "long") {
                continue;
            }
            if (current == option.value) {
                selected = static_cast<guint>(rows->windowValues.size());
            }
            gtk_string_list_append(names.get(), tr(option.label).c_str());
            rows->windowValues.emplace_back(option.value);
        }
        rows->syncing = true;
        adw_combo_row_set_model(ADW_COMBO_ROW(rows->window), G_LIST_MODEL(names.get()));
        adw_combo_row_set_selected(ADW_COMBO_ROW(rows->window), selected);
        rows->syncing = false;
    };
    const auto refresh = [rows, rebuildWindow](const std::string& engine) {
        const bool any       = engine != "disabled";
        const bool varispeed = engine == "varispeed";
        const bool finer     = engine == "finer";
        const bool rubber    = engine == "faster" || finer;
        gtk_widget_set_visible(rows->pitch, any && !varispeed);
        gtk_widget_set_visible(rows->tempo, any);
        gtk_widget_set_visible(rows->lock, any && !varispeed);
        gtk_widget_set_visible(rows->reset, any);
        gtk_widget_set_visible(rows->transients, rubber && !finer);
        gtk_widget_set_visible(rows->detector, rubber && !finer);
        gtk_widget_set_visible(rows->phase, rubber && !finer);
        gtk_widget_set_visible(rows->window, rubber);
        gtk_widget_set_visible(rows->smoothing, rubber && !finer);
        gtk_widget_set_visible(rows->formant, rubber);
        gtk_widget_set_visible(rows->pitchMode, rubber);
        gtk_widget_set_visible(rows->channels, rubber);
        gtk_widget_set_visible(rows->note, varispeed);
        if (rubber) {
            rebuildWindow(finer);
        }
    };

    row->choice(tr("Engine"), "rubberbandEngine", app::kStretchEngineChoices, refresh);

    const auto showValue = [](GtkWidget* label, double ratio) {
        char buffer[24] = {};
        std::snprintf(buffer, sizeof(buffer), "%.2f\xC3\x97", ratio);
        gtk_label_set_text(GTK_LABEL(label), buffer);
    };
    const auto speedRow = [&](const std::string& label, GtkWidget*& scale, GtkWidget*& value,
                              double ratio) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, app::kSpeedSliderMax, 1);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_widget_set_size_request(scale, 220, -1);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_scale_add_mark(GTK_SCALE(scale), app::sliderFromSpeed(1.0), GTK_POS_BOTTOM, nullptr);
        gtk_range_set_value(GTK_RANGE(scale), app::sliderFromSpeed(ratio));
        value = gtk_label_new("");
        gtk_widget_add_css_class(value, "numeric");
        gtk_widget_set_size_request(value, 52, -1);
        showValue(value, ratio);
        gtk_box_append(GTK_BOX(box), scale);
        gtk_box_append(GTK_BOX(box), value);
        return row->add(label, box);
    };
    rows->pitch = speedRow(tr("Pitch"), rows->pitchScale, rows->pitchValue, settings_.Pitch());
    rows->tempo = speedRow(tr("Tempo"), rows->tempoScale, rows->tempoValue, settings_.Tempo());

    const auto applySpeed = [this, rows, showValue](const char* key, GtkWidget* scale,
                                                    GtkWidget* value, const char* otherKey,
                                                    GtkWidget* otherScale, GtkWidget* otherValue) {
        if (rows->syncing) {
            return;
        }
        const double ratio = app::snapSpeed(app::speedFromSlider(
            static_cast<int>(std::lround(gtk_range_get_value(GTK_RANGE(scale))))));
        showValue(value, ratio);
        settings_.setRawValue(key, std::to_string(ratio));
        settingChanged.publish(key);
        if (settings_.SpeedLock()) {
            rows->syncing = true;
            gtk_range_set_value(GTK_RANGE(otherScale), app::sliderFromSpeed(ratio));
            rows->syncing = false;
            showValue(otherValue, ratio);
            settings_.setRawValue(otherKey, std::to_string(ratio));
            settingChanged.publish(otherKey);
        }
    };
    connections_.push_back(Connection::to<void(GtkRange*)>(
        rows->pitchScale, "value-changed", [=](GtkRange*) {
            applySpeed("pitch", rows->pitchScale, rows->pitchValue, "tempo", rows->tempoScale,
                       rows->tempoValue);
        }));
    connections_.push_back(Connection::to<void(GtkRange*)>(
        rows->tempoScale, "value-changed", [=](GtkRange*) {
            applySpeed("tempo", rows->tempoScale, rows->tempoValue, "pitch", rows->pitchScale,
                       rows->pitchValue);
        }));

    rows->lock = row->toggle(tr("Lock pitch and tempo together"), "speedLock",
                             tr("Moving either slider moves both, which is what a record "
                                "player's speed control does."));
    GtkWidget* reset = gtk_button_new_with_label(tr("Reset to 1.00\xC3\x97").c_str());
    connections_.push_back(Connection::to<void(GtkButton*)>(reset, "clicked", [=, this](GtkButton*) {
        rows->syncing = true;
        gtk_range_set_value(GTK_RANGE(rows->pitchScale), app::sliderFromSpeed(1.0));
        gtk_range_set_value(GTK_RANGE(rows->tempoScale), app::sliderFromSpeed(1.0));
        rows->syncing = false;
        showValue(rows->pitchValue, 1.0);
        showValue(rows->tempoValue, 1.0);
        settings_.setRawValue("pitch", "1");
        settings_.setRawValue("tempo", "1");
        settingChanged.publish("pitch");
        settingChanged.publish("tempo");
    }));
    rows->reset = row->buttons({reset});

    rows->transients = row->choice(tr("Transients"), "rubberbandTransients", app::kRubberTransientsChoices);
    rows->detector   = row->choice(tr("Detector"), "rubberbandDetector", app::kRubberDetectorChoices);
    rows->phase      = row->choice(tr("Phase"), "rubberbandPhase", app::kRubberPhaseChoices);
    rows->window     = row->picker(tr("Window"), {}, 0, [this, rows](guint selected) {
        if (rows->syncing) {
            return;
        }
        if (selected < rows->windowValues.size()) {
            settings_.setRawValue("rubberbandWindow", rows->windowValues[selected]);
            settingChanged.publish("rubberbandWindow");
        }
    });
    rows->smoothing = row->choice(tr("Smoothing"), "rubberbandSmoothing", app::kRubberSmoothingChoices);
    rows->formant   = row->choice(tr("Formant"), "rubberbandFormant", app::kRubberFormantChoices);
    rows->pitchMode = row->choice(tr("Pitch mode"), "rubberbandPitch", app::kRubberPitchChoices);
    rows->channels  = row->choice(tr("Channels"), "rubberbandChannels", app::kRubberChannelsChoices);
    rows->note      = row->note(tr("Varispeed resamples, as a record player would: one tempo "
                                   "slider, and the pitch follows it."));
    refresh(settings_.RubberbandEngine());
}

void PreferencesDialog::buildGeneralPage() {
    XPCOG_ROWS("general", tr("General"), "preferences-system-symbolic");

    std::vector<std::string> languageNames;
    std::vector<std::string> languageCodes;
    guint                    chosen = 0;
    for (const app::LanguageOption& option : app::availableLanguages()) {
        if (option.code == settings_.Language()) {
            chosen = static_cast<guint>(languageCodes.size());
        }
        languageNames.push_back(option.code.empty() ? tr("Follow the system") : option.name);
        languageCodes.push_back(option.code);
    }
    row->picker(tr("Language"), languageNames, chosen, [this, languageCodes](guint index) {
        if (index >= languageCodes.size()) {
            return;
        }
        settings_.setLanguage(languageCodes[index]);
        settingChanged.publish("language");
        settings_.sync();
    });
    row->note(tr("Restart XPCog to apply."));

    row->number(tr("Streaming buffer (bytes)"), "httpStreamingBufferSize", 65536, 134217728, 65536);
    row->note(tr("How much of an internet radio stream is read ahead. Raise it for a slow or "
                 "distant station."));

    GtkWidget* consent = row->toggle(tr("Send crash reports and usage data"), "sentryConsented");
    if (platform::crashReportingAvailable()) {
        row->note(tr("Nothing is collected or sent while this is off."));
        row->link(tr("Privacy policy"), platform::kPrivacyPolicyUrl);
    } else {
        gtk_widget_set_sensitive(consent, FALSE);
        row->note(tr("Crash reporting is not included in this build."));
    }

    row->heading(tr("Lyrics"));
    // How, before where from: it applies to the file's own lyrics as much as
    // to LRCLIB's, so it does not belong under the switch that sends
    // something out.
    row->toggle(tr("Follow timed lyrics line by line"), "lyricsSynced");
    row->note(tr("Off, timed lyrics are shown as plain text, and a file's own "
                 "lyrics tag is preferred over a .lrc file beside it."));
    GtkWidget* lyrics = row->toggle(tr("Look up lyrics on LRCLIB when the file has none"), "enableLrclib");
    if (httpClientAvailable()) {
        row->note(tr("The artist, title, album and length of the track you are looking at are "
                     "sent to LRCLIB, a free lyrics service with no accounts. Each answer is kept "
                     "in the library, so a track is asked about once."));
        row->link(tr("About LRCLIB"), "https://lrclib.net/");
        row->text(tr("API address"), "lrclibUrl");
    } else {
        gtk_widget_set_sensitive(lyrics, FALSE);
        row->note(tr("This build was configured without HTTP support, so it cannot reach LRCLIB."));
    }
}

void PreferencesDialog::buildNotificationsPage() {
    XPCOG_ROWS("notifications", tr("Notifications"), "preferences-system-notifications-symbolic");
    row->toggle(tr("Enable notifications"), "notifications.enable");
    row->toggle(tr("Show album art"), "notifications.show-album-art");
    row->note(tr("Shown as each track starts. Focus Assist or Do Not Disturb can hold "
                 "notifications back."));
}

void PreferencesDialog::buildLastFmPage() {
    XPCOG_ROWS("lastfm", "Last.fm", "audio-headphones-symbolic");  // a proper noun
    app::LastFmAccount* account   = session_.lastFm();
    Scrobbler*          scrobbler = session_.scrobbler();

    GtkWidget* enable  = row->toggle(tr("Scrobble to Last.fm"), "enableAudioScrobbler");
    GtkWidget* status  = row->note("", false);
    GtkWidget* connect = gtk_button_new_with_label(tr("Connect...").c_str());
    GtkWidget* cancel  = gtk_button_new_with_label(tr("Cancel").c_str());
    GtkWidget* forget  = gtk_button_new_with_label(tr("Disconnect").c_str());
    row->buttons({connect, cancel, forget});
    row->note(tr("Plays are sent once you have heard half a track, or four minutes of it, "
                 "whichever comes first. Tracks under 30 seconds are never scrobbled."));
    row->link(tr("Your Last.fm applications"), "https://www.last.fm/settings/applications");
    row->note(tr("Revoking access there stops scrobbling immediately, whatever this pane says."));

    row->heading(tr("API account"));
    GtkWidget* keyStatus = row->note("");
    row->link(tr("Create a Last.fm API account"), "https://www.last.fm/api/account/create");
    const app::LastFmAccount::ApiCredentials own = app::LastFmAccount::loadApiCredentials();
    GtkWidget* keyEdit = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(keyEdit), tr("API key").c_str());
    gtk_editable_set_text(GTK_EDITABLE(keyEdit), own.key.c_str());
    row->add(keyEdit);
    GtkWidget* secretEdit = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(secretEdit), tr("Shared secret").c_str());
    gtk_editable_set_text(GTK_EDITABLE(secretEdit), own.secret.c_str());
    row->add(secretEdit);
    GtkWidget* use    = gtk_button_new_with_label(tr("Use this key").c_str());
    GtkWidget* remove = gtk_button_new_with_label(tr("Remove").c_str());
    row->buttons({use, remove});
    row->note(tr("A connection belongs to the key that opened it, so changing the key "
                 "disconnects you and you connect again under the new one."));

    const std::weak_ptr<int> alive = alive_;
    const auto refresh = [=, this] {
        if (alive.expired()) {
            return;
        }
        const bool  built = account->usable();
        std::string problem;
        const bool  store   = app::LastFmAccount::storeAvailable(&problem);
        const bool  working = account->connecting();
        const auto  session = scrobbler->session();
        gtk_widget_set_sensitive(enable, built && store);
        gtk_widget_set_visible(connect, !session.connected() && !working);
        gtk_widget_set_visible(cancel, working);
        gtk_widget_set_visible(forget, session.connected() && !working);
        gtk_widget_set_sensitive(connect, built && store);

        const bool ownKey  = account->usingOwnCredentials();
        const bool canEdit = store && httpClientAvailable() && !working;
        gtk_widget_set_sensitive(keyEdit, canEdit);
        gtk_widget_set_sensitive(secretEdit, canEdit);
        gtk_widget_set_sensitive(use, canEdit);
        gtk_widget_set_visible(remove, ownKey);
        gtk_widget_set_sensitive(remove, canEdit);
        if (ownKey) {
            gtk_label_set_text(GTK_LABEL(keyStatus), tr("Using your own API key.").c_str());
        } else if (app::LastFmAccount::hasBuiltInCredentials()) {
            gtk_label_set_text(GTK_LABEL(keyStatus),
                               tr("Using the key built into XPCog. Enter your own to scrobble as "
                                  "an application of your own.")
                                   .c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(keyStatus),
                               tr("This build carries no API key, so scrobbling needs one of "
                                  "yours.")
                                   .c_str());
        }

        std::string text;
        if (!built) {
            text = account->unavailableReason();
        } else if (!store) {
            text = problem.empty()
                       ? tr("The system password store is not available, so a Last.fm session "
                            "cannot be kept.")
                       : problem;
        } else if (working) {
            text = tr("Waiting for you to allow access in your browser...");
        } else if (session.connected()) {
            text = app::trf("Connected as %s.", session.username);
        } else {
            text = tr("Not connected. Connecting opens Last.fm in your browser; XPCog never sees "
                      "your password.");
        }
        if (const std::size_t waiting = scrobbler->pending(); waiting > 0) {
            text += "\n";
            text += app::fmt(app::trn("%zu play waiting to be sent.", "%zu plays waiting to be sent.",
                                      waiting),
                             waiting);
        }
        gtk_label_set_text(GTK_LABEL(status), text.c_str());
    };

    connections_.push_back(Connection::to<void(GtkButton*)>(
        connect, "clicked", [=, this](GtkButton*) {
            app::LastFmAccount::ConnectHandlers handlers;
            handlers.awaitingAuthorization = [refresh](const std::string&) { refresh(); };
            handlers.connected = [refresh, scrobbler, this](const Scrobbler::Session& session) {
                scrobbler->setSession(session);
                settings_.setEnableScrobbling(true);
                refresh();
            };
            handlers.failed = [refresh, alive, status](const std::string& message) {
                refresh();
                if (!alive.expired()) {
                    gtk_label_set_text(GTK_LABEL(status), message.c_str());
                }
            };
            account->connect(&postToMainContext, std::move(handlers));
            refresh();
        }));
    connections_.push_back(Connection::to<void(GtkButton*)>(cancel, "clicked", [=](GtkButton*) {
        account->cancelConnect();
        refresh();
    }));
    connections_.push_back(Connection::to<void(GtkButton*)>(forget, "clicked", [=](GtkButton*) {
        account->forget();
        scrobbler->setSession({});
        refresh();
    }));

    const auto apply = [=, this](app::LastFmAccount::ApiCredentials credentials, bool removing) {
        const auto result = (!removing && !credentials.complete())
                                ? app::LastFmAccount::ApplyResult::Incomplete
                                : account->setApiCredentials(std::move(credentials));
        std::string keyMessage;
        std::string connectionMessage;
        switch (result) {
            case app::LastFmAccount::ApplyResult::Applied:
                break;
            case app::LastFmAccount::ApplyResult::AppliedAndDisconnected:
                scrobbler->setSession({});
                connectionMessage = tr("The key changed, so you have been disconnected. Connect "
                                       "again to scrobble under it.");
                break;
            case app::LastFmAccount::ApplyResult::Incomplete:
                keyMessage = tr("Both the API key and the shared secret are needed.");
                break;
            case app::LastFmAccount::ApplyResult::StoreRefused:
                keyMessage = tr("The system password store would not keep the key.");
                break;
        }
        refresh();
        if (alive.expired()) {
            return;
        }
        if (!keyMessage.empty()) {
            gtk_label_set_text(GTK_LABEL(keyStatus), keyMessage.c_str());
        }
        if (!connectionMessage.empty()) {
            gtk_label_set_text(GTK_LABEL(status), connectionMessage.c_str());
        }
        const auto stored = app::LastFmAccount::loadApiCredentials();
        gtk_editable_set_text(GTK_EDITABLE(keyEdit), stored.key.c_str());
        gtk_editable_set_text(GTK_EDITABLE(secretEdit), stored.secret.c_str());
    };
    connections_.push_back(Connection::to<void(GtkButton*)>(use, "clicked", [=](GtkButton*) {
        app::LastFmAccount::ApiCredentials credentials;
        credentials.key    = trimmed(gtk_editable_get_text(GTK_EDITABLE(keyEdit)));
        credentials.secret = trimmed(gtk_editable_get_text(GTK_EDITABLE(secretEdit)));
        apply(std::move(credentials), /*removing=*/false);
    }));
    connections_.push_back(Connection::to<void(GtkButton*)>(remove, "clicked", [=](GtkButton*) {
        apply(app::LastFmAccount::ApiCredentials{}, /*removing=*/true);
    }));
    refresh();
}

void PreferencesDialog::buildListenBrainzPage() {
    XPCOG_ROWS("listenbrainz", "ListenBrainz", "audio-headphones-symbolic");  // likewise
    app::ListenBrainzAccount* account   = session_.listenBrainz();
    Scrobbler*                scrobbler = session_.listenBrainzScrobbler();

    GtkWidget* enable    = row->toggle(tr("Scrobble to ListenBrainz"), "enableListenBrainz");
    GtkWidget* status    = row->note("", false);
    GtkWidget* tokenEdit = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tokenEdit), tr("User token").c_str());
    row->add(tokenEdit);
    GtkWidget* tokenLink = row->link(tr("Your ListenBrainz user token"), "https://listenbrainz.org/settings/");
    GtkWidget* connect   = gtk_button_new_with_label(tr("Connect").c_str());
    GtkWidget* forget    = gtk_button_new_with_label(tr("Disconnect").c_str());
    row->buttons({connect, forget});
    row->note(tr("Plays are sent once you have heard half a track, or four minutes of it, "
                 "whichever comes first. Tracks under 30 seconds are never scrobbled."));
    row->heading(tr("Server"));
    row->text(tr("API address"), "listenBrainzUrl");
    row->note(tr("The public service by default. A ListenBrainz you run yourself, or Maloja, "
                 "takes the same requests at its own address."));

    const std::weak_ptr<int> alive = alive_;
    const auto refresh = [=] {
        if (alive.expired()) {
            return;
        }
        const bool  built = account->usable();
        std::string problem;
        const bool  store   = app::LastFmAccount::storeAvailable(&problem);
        const bool  working = account->connecting();
        const auto  session = scrobbler->session();
        const bool  ready   = built && store;
        gtk_widget_set_sensitive(enable, ready);
        gtk_widget_set_visible(tokenEdit, !session.connected());
        gtk_widget_set_visible(tokenLink, !session.connected());
        gtk_widget_set_sensitive(tokenEdit, ready && !working);
        gtk_widget_set_visible(connect, !session.connected());
        gtk_widget_set_sensitive(connect, ready && !working);
        gtk_widget_set_visible(forget, session.connected());
        gtk_widget_set_sensitive(forget, !working);

        std::string text;
        if (!built) {
            text = account->unavailableReason();
        } else if (!store) {
            text = problem.empty()
                       ? tr("The system password store is not available, so a ListenBrainz token "
                            "cannot be kept.")
                       : problem;
        } else if (working) {
            text = tr("Checking the token with ListenBrainz...");
        } else if (session.connected()) {
            text = app::trf("Connected as %s.", session.username);
        } else {
            text = tr("Not connected. Paste the user token from your ListenBrainz settings page "
                      "and press Connect.");
        }
        if (const std::size_t waiting = scrobbler->pending(); waiting > 0) {
            text += "\n";
            text += app::fmt(app::trn("%zu play waiting to be sent.", "%zu plays waiting to be sent.",
                                      waiting),
                             waiting);
        }
        gtk_label_set_text(GTK_LABEL(status), text.c_str());
    };
    const auto startConnect = [=, this] {
        const std::string typed = trimmed(gtk_editable_get_text(GTK_EDITABLE(tokenEdit)));
        app::ListenBrainzAccount::ConnectHandlers handlers;
        handlers.connected = [refresh, scrobbler, alive, tokenEdit, this](const Scrobbler::Session& session) {
            scrobbler->setSession(session);
            settings_.setEnableListenBrainz(true);
            if (!alive.expired()) {
                gtk_editable_set_text(GTK_EDITABLE(tokenEdit), "");
            }
            refresh();
        };
        handlers.failed = [refresh, alive, status](const std::string& message) {
            refresh();
            if (!alive.expired()) {
                gtk_label_set_text(GTK_LABEL(status), message.c_str());
            }
        };
        account->connect(typed, &postToMainContext, std::move(handlers));
        refresh();
    };
    connections_.push_back(
        Connection::to<void(GtkButton*)>(connect, "clicked", [=](GtkButton*) { startConnect(); }));
    connections_.push_back(Connection::to<void(AdwEntryRow*)>(
        tokenEdit, "entry-activated", [=](AdwEntryRow*) { startConnect(); }));
    connections_.push_back(Connection::to<void(GtkButton*)>(forget, "clicked", [=](GtkButton*) {
        account->forget();
        scrobbler->setSession({});
        refresh();
    }));
    refresh();
}

void PreferencesDialog::buildAppearancePage() {
    XPCOG_ROWS("appearance", tr("Appearance"), "preferences-desktop-appearance-symbolic");

    GtkWidget* closeToTray = row->toggle(tr("Close to tray"), "closeToTray",
                                         tr("Closing the window leaves XPCog running in the "
                                            "notification area instead of quitting."));
    // Offered only where there is somewhere to hide to, as the wx dialog
    // does: a checkbox that does nothing is worse than an absent one.
    if (!hasTray_) {
        gtk_widget_set_sensitive(closeToTray, FALSE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(closeToTray),
                                    tr("This session has no notification area to keep XPCog in.").c_str());
    }
    // Dead here, and said so where the row is rather than left as a switch that
    // does nothing: GTK 4 has no keep-above, and Wayland would not honour one.
    GtkWidget* floating = row->toggle(tr("Keep the mini player on top"), "floatingMiniWindow");
    gtk_widget_set_sensitive(floating, FALSE);

    GtkWidget* show = row->toggle(tr("Show the waveform in the seek bar"), "waveformSeekBar",
                                  tr("The track's shape behind the playhead, analysed the first "
                                     "time it plays and kept for every play after."));
    GtkWidget* rectified = row->toggle(tr("Rectified: stand the waveform on the bottom edge"),
                                       "waveformRectified",
                                       tr("Instead of mirroring it about the centre line."));
    GtkWidget* logarithmic = row->toggle(tr("Logarithmic: draw levels in decibels"),
                                         "waveformLogScale",
                                         tr("So quiet material is a shape rather than a line."));
    GtkWidget* height = row->number(tr("Height"), "waveformHeight", 20, 80);

    const auto colourWithDefault = [&](const std::string& follow, const std::string& pick,
                                       const char* key, const char* fallback) {
        GtkWidget* follows = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(follows), follow.c_str());
        adw_switch_row_set_active(ADW_SWITCH_ROW(follows), settings_.rawValue(key).empty());
        row->add(follows);

        GdkRGBA rgba;
        if (!gdk_rgba_parse(&rgba, settings_.rawValue(key).c_str())) {
            gdk_rgba_parse(&rgba, fallback);
        }
        GtkWidget* button = gtk_color_dialog_button_new(gtk_color_dialog_new());
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            button, "notify::rgba", [this, key, button](GObject*, GParamSpec*) {
                settings_.setRawValue(
                    key, hexOf(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button))));
                settingChanged.publish(key);
            }));
        GtkWidget* pickerRow = row->add(pick, button);
        gtk_widget_set_sensitive(pickerRow, !adw_switch_row_get_active(ADW_SWITCH_ROW(follows)));
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            follows, "notify::active", [this, key, follows, pickerRow, button](GObject*, GParamSpec*) {
                const bool following = adw_switch_row_get_active(ADW_SWITCH_ROW(follows));
                gtk_widget_set_sensitive(pickerRow, !following);
                // Following: the key is emptied and the theme answers. Not:
                // the picker's colour, which is the fallback until it is moved.
                settings_.setRawValue(
                    key, following ? std::string{}
                                   : hexOf(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button))));
                settingChanged.publish(key);
            }));
        return std::pair{follows, pickerRow};
    };
    const auto played   = colourWithDefault(tr("Played part follows the system accent colour"),
                                            tr("Played colour"), "waveformPlayedColor", "#0a84ff");
    const auto unplayed = colourWithDefault(tr("Unplayed part follows the theme's text colour"),
                                            tr("Unplayed colour"), "waveformUnplayedColor", "#808080");
    const auto styles = [=](bool on) {
        gtk_widget_set_sensitive(rectified, on);
        gtk_widget_set_sensitive(logarithmic, on);
        gtk_widget_set_sensitive(height, on);
        for (const auto& pair : {played, unplayed}) {
            gtk_widget_set_sensitive(pair.first, on);
            gtk_widget_set_sensitive(pair.second,
                                     on && !adw_switch_row_get_active(ADW_SWITCH_ROW(pair.first)));
        }
    };
    styles(settings_.WaveformSeekBar());
    connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
        show, "notify::active", [=](GObject*, GParamSpec*) {
            styles(adw_switch_row_get_active(ADW_SWITCH_ROW(show)));
        }));
}

void PreferencesDialog::buildMidiPage() {
    XPCOG_ROWS("midi", "MIDI", "audio-card-symbolic");  // an acronym, the same in every language

    struct Rows {
        GtkWidget *soundFont = nullptr, *roms = nullptr, *spessaNote = nullptr, *sc55Note = nullptr;
    };
    auto rows = std::make_shared<Rows>();
    keep_.push_back(rows);
    const auto refresh = [rows](const std::string& synth) {
        const bool spessa = synth == "Spessa";
        const bool sc55   = synth == "NukeSc55";
        gtk_widget_set_visible(rows->soundFont, spessa);
        gtk_widget_set_visible(rows->spessaNote, spessa);
        gtk_widget_set_visible(rows->roms, sc55);
        gtk_widget_set_visible(rows->sc55Note, sc55);
    };
    row->choice(tr("Synthesiser"), "midiPlugin", app::kMidiSynthChoices, refresh);
    rows->soundFont = row->path(tr("SoundFont"), "soundFontPath", false, true,
                                {"*.sf2", "*.sf3", "*.sf2pack", "*.dls", "*.sflist", "*.json"});
    rows->roms = row->path(tr("SC-55 ROMs"), "midiRomPath", true, true, {"*.zip", "*.rar", "*.7z"});
    row->number(tr("Sample rate (Hz)"), "synthSampleRate", 8000, 192000, 100);
    row->number(tr("Default play time (s)"), "synthDefaultSeconds", 0.0, 3600.0, 0.1, 1);
    row->number(tr("Default fade time (s)"), "synthDefaultFadeSeconds", 0.0, 60.0, 0.1, 1);
    row->number(tr("Default loop count"), "synthDefaultLoopCount", 0, 10);
    rows->spessaNote = row->note(tr("SpessaSynth needs a bank: any .sf2, .sf3 or .dls. Files that "
                                    "carry their own bank use that instead."));
    rows->sc55Note   = row->note(tr("The SC-55 needs its five ROM files, which are not supplied. "
                                    "Choose the folder or the archive they came in; they are "
                                    "recognised by content, so nothing needs renaming. Without "
                                    "them, MIDI plays on the OPL3. It also ignores the sample rate "
                                    "and always renders at its own."));
    row->note(tr("These apply to every synthesised format, not only MIDI."));
    refresh(settings_.MidiPlugin());
}

void PreferencesDialog::buildVisualizersPage() {
    XPCOG_ROWS("visualizers", tr("Visualizers"), "view-grid-symbolic");

    row->heading(tr("Spectrum"));
    row->picker(tr("Bands"),
                {tr("Musical notes (one bar per semitone)"), tr("Even frequency spacing")},
                settings_.SpectrumFreqMode() ? 1 : 0, [this](guint index) {
                    settings_.setSpectrumFreqMode(index == 1);
                    settingChanged.publish("spectrumFreqMode");
                });
    static constexpr std::array kSpectrumChannels = {
        Choice{"mono", XPCOG_TRANSLATE("Mono")},
        Choice{"left", XPCOG_TRANSLATE("Left")},
        Choice{"right", XPCOG_TRANSLATE("Right")},
        Choice{"mirrored", XPCOG_TRANSLATE("Stereo, mirrored")},
        Choice{"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
        Choice{"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
    };
    row->choice(tr("Channels"), "spectrumChannels", kSpectrumChannels);
    row->colour(tr("Bar colour"), "spectrumBarColor", "#ff8000");
    row->colour(tr("Peak colour"), "spectrumDotColor", "#ff3b30");
    row->toggle(tr("Show peak markers"), "spectrumShowPeaks");
    row->number(tr("Quietest level shown (dB)"), "spectrumFloorDb", -120, -20);
    row->note(tr("Bars sit on semitones from C0, so the display lines up with the notes being "
                 "played."));

    row->heading(tr("Oscilloscope"));
    static constexpr std::array kChannels = {
        Choice{"mono", XPCOG_TRANSLATE("Mono")},
        Choice{"left", XPCOG_TRANSLATE("Left")},
        Choice{"right", XPCOG_TRANSLATE("Right")},
        Choice{"stacked", XPCOG_TRANSLATE("Stereo, stacked")},
        Choice{"overlaid", XPCOG_TRANSLATE("Stereo, overlaid")},
    };
    row->choice(tr("Channels"), "scopeChannels", kChannels);
    row->colour(tr("Trace colour"), "scopeColor", "#30d158");
    row->colour(tr("Background"), "scopeBackgroundColor", "#121214");
    row->number(tr("Stroke width"), "scopeStrokeWidth", 0.5, 6.0, 0.5, 1);
    row->number(tr("Vertical gain"), "scopeGain", 0.25, 8.0, 0.25, 2);
    row->number(tr("Window (ms)"), "scopeWindowMs", 5, 100);
    row->number(tr("Frames per second"), "scopeFrameRate", 15, 120);
    row->toggle(tr("Hold a steady tone still"), "scopeTrigger",
                tr("Start each frame at a rising zero crossing, so a tone does not crawl across "
                   "the display."));
    row->toggle(tr("Fill under the trace"), "scopeFill");
    row->toggle(tr("Logarithmic scale"), "scopeLogScale",
                tr("Levels in decibels rather than linear, so quiet material is a shape rather "
                   "than a line."));
}

void PreferencesDialog::buildRemotePage() {
    XPCOG_ROWS("remote", tr("Remote"), "network-server-symbolic");

    GtkWidget* enable = row->toggle(tr("Allow remote control over HTTP"), "remoteEnable");
    static constexpr std::array kAddresses = std::to_array<Choice>({
        {"127.0.0.1", XPCOG_TRANSLATE("This computer only")},
        {"0.0.0.0", XPCOG_TRANSLATE("Any computer on the network")},
    });
    row->choice(tr("Listen on"), "remoteAddress", kAddresses);
    row->number(tr("Port"), "remotePort", 1024, 65535);
    row->toggle(tr("Allow changes, not just reading"), "remoteAllowWrite");
    GtkWidget* local = row->toggle(tr("Let programs on this computer connect without the token"),
                                   "remoteLoopbackNoToken");
    row->note(tr("For a script or a command line driving your own player. Any program on this "
                 "computer can then control it, including a web page that was told to try. "
                 "Other computers still need the token."));

    GtkWidget* token = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(token), tr("Access token").c_str());
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(token), TRUE);
    gtk_widget_add_css_class(token, "property");
    GtkWidget* copy       = gtk_button_new_with_label(tr("Copy").c_str());
    GtkWidget* regenerate = gtk_button_new_with_label(tr("Regenerate").c_str());
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(regenerate, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(token), copy);
    adw_action_row_add_suffix(ADW_ACTION_ROW(token), regenerate);
    row->add(token);
    GtkWidget* status = row->note("", false);

    const auto refresh = [=, this] {
        const bool  built = remote::remoteServerAvailable();
        std::string problem;
        const bool  store = app::RemoteToken::storeAvailable(&problem);
        for (GtkWidget* widget : {enable, local, token, copy, regenerate}) {
            gtk_widget_set_sensitive(widget, built && store);
        }
        std::string text;
        if (!built) {
            text = tr("This build has no remote-control server.");
        } else if (!store) {
            text = problem.empty() ? tr("The system password store is not available, so an access "
                                        "token cannot be kept safely.")
                                   : problem;
        } else {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(token), app::RemoteToken::load().c_str());
            if (!isTrue(settings_.rawValue("remoteEnable"))) {
                text = tr("Off. Nothing is listening.");
            } else {
                const std::string address = settings_.rawValue("remoteAddress");
                const std::string where =
                    "http://" + (address == "0.0.0.0" ? std::string("<this computer>") : address) +
                    ":" + settings_.rawValue("remotePort");
                text = app::trf("Listening on %s -- open %s/docs in a browser to try it.", where, where);
                if (isTrue(settings_.rawValue("remoteLoopbackNoToken"))) {
                    text += "\n\n";
                    text += tr("Programs on this computer are connecting without the token.");
                }
                if (address == "0.0.0.0") {
                    text += "\n\n";
                    text += tr("Reachable from other machines. The connection is not encrypted and "
                               "the access token is sent with every request, so use this on a "
                               "network you trust.");
                }
            }
        }
        gtk_label_set_text(GTK_LABEL(status), text.c_str());
    };
    for (GtkWidget* widget : {enable, local}) {
        connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
            widget, "notify::active", [refresh](GObject*, GParamSpec*) { refresh(); }));
    }
    connections_.push_back(Connection::to<void(GtkButton*)>(copy, "clicked", [token](GtkButton* b) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)),
                               adw_action_row_get_subtitle(ADW_ACTION_ROW(token)));
    }));
    connections_.push_back(Connection::to<void(GtkButton*)>(
        regenerate, "clicked", [=, this](GtkButton* b) {
            AdwDialog* ask = adw_alert_dialog_new(
                tr("Regenerate Access Token").c_str(),
                tr("Every device using the current token will stop working until it is given the "
                   "new one.\n\nGenerate a new token?")
                    .c_str());
            adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(ask), "cancel", tr("Cancel").c_str(),
                                           "regenerate", tr("Regenerate").c_str(), nullptr);
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(ask), "regenerate",
                                                     ADW_RESPONSE_DESTRUCTIVE);
            adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(ask), "cancel");
            struct Pending {
                PreferencesDialog*    self;
                std::function<void()> refresh;
                std::weak_ptr<int>    alive;
            };
            auto* pending = new Pending{this, refresh, alive_};
            adw_alert_dialog_choose(
                ADW_ALERT_DIALOG(ask), GTK_WIDGET(b), nullptr,
                [](GObject* source, GAsyncResult* result, gpointer data) {
                    std::unique_ptr<Pending> p(static_cast<Pending*>(data));
                    const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
                    if (g_strcmp0(response, "regenerate") != 0 || p->alive.expired()) {
                        return;
                    }
                    static_cast<void>(app::RemoteToken::regenerate());
                    p->self->settingChanged.publish("remoteEnable");
                    p->refresh();
                },
                pending);
        }));
    refresh();
}

void PreferencesDialog::buildAdvancedPage() {
    XPCOG_ROWS("advanced", tr("Advanced"), "applications-engineering-symbolic");

    for (const Settings::Desc& descriptor : Settings::all()) {
        if (app::hasCuratedRow(descriptor.key)) {
            continue;
        }
        const std::string key(descriptor.key);
        const std::string label(descriptor.ident);
        const std::string value = settings_.rawValue(key);
        GtkWidget*        made  = nullptr;
        if (descriptor.type == "bool") {
            made = adw_switch_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(made), label.c_str());
            adw_switch_row_set_active(ADW_SWITCH_ROW(made), isTrue(value));
            connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
                made, "notify::active", [this, key, made](GObject*, GParamSpec*) {
                    settings_.setRawValue(key, adw_switch_row_get_active(ADW_SWITCH_ROW(made)) ? "true"
                                                                                                : "false");
                    settingChanged.publish(key);
                }));
        } else if (descriptor.type == "int" || descriptor.type == "double") {
            const bool whole = descriptor.type == "int";
            made = adw_spin_row_new_with_range(-1000000.0, 1000000.0, whole ? 1.0 : 0.001);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(made), label.c_str());
            adw_spin_row_set_digits(ADW_SPIN_ROW(made), whole ? 0 : 3);
            adw_spin_row_set_value(ADW_SPIN_ROW(made), whole ? toInt(value) : toDouble(value));
            connections_.push_back(Connection::to<void(GObject*, GParamSpec*)>(
                made, "notify::value", [this, key, made, whole](GObject*, GParamSpec*) {
                    const double v = adw_spin_row_get_value(ADW_SPIN_ROW(made));
                    settings_.setRawValue(key, whole ? std::to_string(static_cast<long long>(std::lround(v)))
                                                     : std::to_string(v));
                    settingChanged.publish(key);
                }));
        } else {
            made = adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(made), label.c_str());
            gtk_editable_set_text(GTK_EDITABLE(made), value.c_str());
            adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(made), TRUE);
            connections_.push_back(Connection::to<void(AdwEntryRow*)>(
                made, "apply", [this, key](AdwEntryRow* entry) {
                    settings_.setRawValue(key, gtk_editable_get_text(GTK_EDITABLE(entry)));
                    settingChanged.publish(key);
                }));
        }
        if (app::isInternalKey(descriptor.key)) {
            gtk_widget_set_sensitive(made, FALSE);
            gtk_widget_set_tooltip_text(made, tr("Maintained automatically, and not meant to be edited.").c_str());
        }
        row->add(made);
    }
    row->note(tr("The greyed rows are what XPCog remembers about the last session, not settings."));
}

#undef XPCOG_ROWS

}  // namespace xpcog::gtk
