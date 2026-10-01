#include "Dialogs.hpp"

#include "Accelerators.hpp"
#include "Commands.hpp"
#include "Credits.hpp"

#include "Glib.hpp"
#include "Translations.hpp"
#include "UrlHistory.hpp"

#include "xpcog/core/Version.hpp"

#include <adwaita.h>

#include <filesystem>
#include <map>
#include <memory>

namespace xpcog::gtk {

using app::fmt;
using app::tr;
using app::trf;
using app::trn;

void showWarning(GtkWidget* parent, const std::string& heading, const std::string& body) {
    AdwDialog* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "ok", tr("OK").c_str(), nullptr);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_dialog_present(dialog, parent);
}

// --- Open URL ---------------------------------------------------------------------

namespace {

struct OpenUrlState {
    Settings*                       settings;
    std::function<void(const Url&)> open;
    GtkWidget*                      entry = nullptr;
    GtkWidget*                      dialog = nullptr;
    std::vector<Connection>         connections;

    void submit() {
        const std::string text = app::trimUrl(gtk_editable_get_text(GTK_EDITABLE(entry)));
        if (text.empty()) {
            return;
        }
        // Validated against the same parser that will be asked to open it. A
        // more permissive check would accept a bare path as a relative URL, so
        // a mistyped address would be taken here and fail silently later.
        const std::optional<Url> url = Url::parse(text);
        if (!url) {
            showWarning(dialog, tr("Invalid URL"),
                        app::trf("\xE2\x80\x9C%s\xE2\x80\x9D is not an address XPCog can open. It "
                                 "needs a scheme, such as https:// or file://.",
                                 text));
            return;
        }
        settings->setUrlHistory(app::joinUrlHistory(
            app::urlHistoryWith(app::urlHistoryFrom(settings->UrlHistory()), text)));
        adw_dialog_close(ADW_DIALOG(dialog));
        open(*url);
    }
};

}  // namespace

void showOpenUrlDialog(GtkWindow* parent, Settings& settings, std::function<void(const Url&)> open) {
    auto state      = std::make_shared<OpenUrlState>();
    state->settings = &settings;
    state->open     = std::move(open);

    AdwDialog* dialog = adw_dialog_new();
    state->dialog     = GTK_WIDGET(dialog);
    adw_dialog_set_title(dialog, tr("Open URL").c_str());
    adw_dialog_set_content_width(dialog, 480);

    GtkWidget* view   = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(column, 12);
    gtk_widget_set_margin_bottom(column, 12);
    gtk_widget_set_margin_start(column, 12);
    gtk_widget_set_margin_end(column, 12);

    GtkWidget* group = adw_preferences_group_new();
    state->entry     = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state->entry),
                                  tr("Address of a stream or file:").c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), state->entry);
    gtk_box_append(GTK_BOX(column), group);

    // The history, most recent first, each row filling the entry.
    const std::vector<std::string> history = app::urlHistoryFrom(settings.UrlHistory());
    if (!history.empty()) {
        GtkWidget* recent = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(recent), tr("Recent").c_str());
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            GtkWidget* row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), it->c_str());
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            const std::string url = *it;
            state->connections.push_back(Connection::to<void(AdwActionRow*)>(
                row, "activated", [state, url](AdwActionRow*) {
                    gtk_editable_set_text(GTK_EDITABLE(state->entry), url.c_str());
                    gtk_widget_grab_focus(state->entry);
                }));
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(recent), row);
        }
        gtk_box_append(GTK_BOX(column), recent);
        gtk_editable_set_text(GTK_EDITABLE(state->entry), history.back().c_str());
    }

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget* cancel = gtk_button_new_with_label(tr("Cancel").c_str());
    GtkWidget* ok     = gtk_button_new_with_label(tr("Open").c_str());
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);
    gtk_box_append(GTK_BOX(column), buttons);

    state->connections.push_back(Connection::to<void(GtkButton*)>(
        cancel, "clicked", [dialog](GtkButton*) { adw_dialog_close(dialog); }));
    state->connections.push_back(
        Connection::to<void(GtkButton*)>(ok, "clicked", [state](GtkButton*) { state->submit(); }));
    state->connections.push_back(Connection::to<void(AdwEntryRow*)>(
        state->entry, "entry-activated", [state](AdwEntryRow*) { state->submit(); }));
    adw_dialog_set_default_widget(dialog, ok);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), column);
    adw_dialog_set_child(dialog, view);

    // The state lives as long as the dialog: a reference from the object's
    // data, released when the dialog is destroyed.
    g_object_set_data_full(
        G_OBJECT(dialog), "xpcog-state", new std::shared_ptr<OpenUrlState>(state),
        [](gpointer data) { delete static_cast<std::shared_ptr<OpenUrlState>*>(data); });

    adw_dialog_present(dialog, GTK_WIDGET(parent));
    gtk_widget_grab_focus(state->entry);
}

// --- Save Playlist ------------------------------------------------------------------

namespace {

struct SaveState {
    app::Session*        session;
    std::vector<TrackId> selection;
    GtkWindow*           parent;
};

}  // namespace

void showSavePlaylistDialog(GtkWindow* parent, app::Session& session, std::vector<TrackId> selection) {
    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
    gtk_file_dialog_set_title(dialog.get(), tr("Save Playlist").c_str());
    // A different default name for a selection, so two saves in a row do not
    // offer to overwrite each other by accident.
    gtk_file_dialog_set_initial_name(dialog.get(), selection.empty() ? "playlist.m3u8" : "selection.m3u8");

    auto filters = GObjectPtr<GListStore>::adopt(g_list_store_new(GTK_TYPE_FILE_FILTER));
    const std::pair<const char*, const char*> formats[] = {
        {"M3U Playlist (*.m3u8)", "*.m3u8"},
        {"PLS Playlist (*.pls)", "*.pls"},
        {"XSPF Playlist (*.xspf)", "*.xspf"},
    };
    for (const auto& [name, pattern] : formats) {
        auto filter = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
        gtk_file_filter_set_name(filter.get(), tr(name).c_str());
        gtk_file_filter_add_pattern(filter.get(), pattern);
        g_list_store_append(filters.get(), filter.get());
    }
    gtk_file_dialog_set_filters(dialog.get(), G_LIST_MODEL(filters.get()));

    auto* state = new SaveState{&session, std::move(selection), parent};
    gtk_file_dialog_save(
        dialog.release(), parent, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<SaveState> s(static_cast<SaveState*>(data));
            auto owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
            GErrorPtr error;
            auto file = GObjectPtr<GFile>::adopt(gtk_file_dialog_save_finish(owned.get(), result, &error.value));
            if (!file) {
                return;
            }
            GStr path(g_file_get_path(file.get()));
            if (!path) {
                return;
            }
            if (!s->session->savePlaylist(std::filesystem::path{path.c_str()},
                                          s->selection.empty() ? nullptr : &s->selection)) {
                showWarning(GTK_WIDGET(s->parent), "XPCog", tr("Could not write the playlist."));
            }
        },
        state);
}

void showShortcutsDialog(GtkWidget* parent) {
    AdwDialog* dialog = adw_shortcuts_dialog_new();

    // The menus' own grouping and order, which is how a reader already knows
    // where a command lives. A menu with no shortcuts gets no section.
    AdwShortcutsSection* section = nullptr;
    int                  rows    = 0;
    const auto           flush   = [&] {
        if (section != nullptr && rows > 0) {
            adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), section);
        } else if (section != nullptr) {
            g_object_unref(section);
        }
        section = nullptr;
        rows    = 0;
    };
    for (const app::MenuItem& item : app::menuLayout()) {
        if (item.menu != nullptr) {
            flush();
            section = adw_shortcuts_section_new(app::stripMnemonics(app::tr(item.menu)).c_str());
        }
        if (section == nullptr || *item.accelerator == '\0' || !offeredHere(item.id)) {
            continue;
        }
        adw_shortcuts_section_add(
            section, adw_shortcuts_item_new_from_action(app::commandLabel(item.id).c_str(),
                                                        detailedActionName(item.id).c_str()));
        ++rows;
    }
    flush();

    // And this dialog's own key, which belongs to no menu of the table.
    AdwShortcutsSection* general = adw_shortcuts_section_new(app::tr("General").c_str());
    adw_shortcuts_section_add(general, adw_shortcuts_item_new_from_action(
                                           app::stripMnemonics(app::tr("&Keyboard Shortcuts")).c_str(),
                                           "win.shortcuts"));
    adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), general);

    adw_dialog_present(dialog, parent);
}

// --- About ----------------------------------------------------------------------------

namespace {

/// The toolkit's own licence for an SPDX identifier it knows, so the Legal page
/// links the real text; anything else is shown as written.
[[nodiscard]] GtkLicense licenceType(std::string_view spdx) {
    static const std::map<std::string_view, GtkLicense> known = {
        {"MIT", GTK_LICENSE_MIT_X11},
        {"Apache-2.0", GTK_LICENSE_APACHE_2_0},
        {"BSD-3-Clause", GTK_LICENSE_BSD_3},
        {"BSD-2-Clause", GTK_LICENSE_BSD},
        {"GPL-2.0-or-later", GTK_LICENSE_GPL_2_0},
        {"GPL-3.0-or-later", GTK_LICENSE_GPL_3_0},
        {"GPL-3.0-only", GTK_LICENSE_GPL_3_0_ONLY},
        {"LGPL-2.1-or-later", GTK_LICENSE_LGPL_2_1},
        {"MPL-2.0", GTK_LICENSE_MPL_2_0},
    };
    const auto found = known.find(spdx);
    return found == known.end() ? GTK_LICENSE_CUSTOM : found->second;
}

/// One Legal-page entry per component, labelled line by line. The toolkit
/// adds its own link to the licence text below for the licences it knows;
/// for the rest the "Licence:" line is the whole statement, so nothing more
/// is passed. Data rows are labelled as data: "Library: YRW801 sample ROM,
/// Licence: Yamaha" would be wrong twice.
void addLegal(AdwAboutDialog* dialog, std::span<const app::Component> components, bool data) {
    for (const app::Component& component : components) {
        const std::string details =
            data ? trf("Data: %s", component.name) + "\n" +
                       trf("Used for: %s", tr(component.purpose).c_str()) + "\n" +
                       trf("Credit: %s", component.licence)
                 : trf("Library: %s", component.name) + "\n" +
                       trf("Feature: %s", tr(component.purpose).c_str()) + "\n" +
                       trf("Licence: %s", component.licence);
        const GtkLicense type = licenceType(component.licence);
        adw_about_dialog_add_legal_section(dialog, component.name, details.c_str(),
                                           type == GTK_LICENSE_CUSTOM ? GTK_LICENSE_UNKNOWN
                                                                      : type,
                                           nullptr);
    }
}

/// What this build plays, by decoder, in the order a file is offered to them:
/// the wx dialog's Formats tab, as the Troubleshooting page's text.
[[nodiscard]] std::string formatsText(const PluginRegistry& registry) {
    std::string text = "XPCog " + std::string(kVersionString) + "\n\n";
    text += fmt(trn("%zu decoder is compiled in. A file goes to the first row "
                              "below that claims its extension:",
                              "%zu decoders are compiled in. A file goes to the first row "
                              "below that claims its extension:",
                              registry.decoderCount()),
                     registry.decoderCount());
    text += "\n\n";
    for (const DecoderDescriptor& decoder : registry.decoders()) {
        text += decoder.name;
        text += ": ";
        std::string extensions;
        for (const std::string_view extension : decoder.extensions) {
            if (!extensions.empty()) {
                extensions += ' ';
            }
            extensions += extension;
        }
        text += extensions.empty() ? tr("chosen by scheme or MIME type") : extensions;
        text += '\n';
    }
    return text;
}

}  // namespace

void showAboutDialog(GtkWidget* parent, const PluginRegistry& registry) {
    AdwDialog* about  = adw_about_dialog_new();
    auto*      dialog = ADW_ABOUT_DIALOG(about);
    adw_about_dialog_set_application_name(dialog, "XPCog");
    adw_about_dialog_set_application_icon(dialog, "co.losno.XPCog");
    adw_about_dialog_set_version(dialog, std::string(kVersionString).c_str());
    adw_about_dialog_set_comments(dialog, tr("An audio player for Windows and Linux.").c_str());
    adw_about_dialog_set_website(dialog, std::string(kProjectUrl).c_str());
    adw_about_dialog_set_issue_url(dialog, (std::string(kProjectUrl) + "/issues").c_str());
    const char* developers[] = {"Kevin L\xC3\xB3pez Brante", nullptr};
    adw_about_dialog_set_developers(dialog, developers);
    adw_about_dialog_set_copyright(
        dialog, tr("Copyright \xC2\xA9 2026 the XPCog authors.").c_str());
    adw_about_dialog_set_license_type(dialog, GTK_LICENSE_GPL_3_0);

    // Cog first among the thanks, because most of what is under the
    // interface is still theirs -- the reason the wx About names them in its
    // copyright lines too.
    const char* cog[] = {"Vincent Spader", "Christopher Snowhill",
                         "The Cog authors https://cog.losno.co", nullptr};
    adw_about_dialog_add_acknowledgement_section(dialog, tr("Ported from Cog").c_str(), cog);

    // Every component, on the Legal page, from the table both frontends share.
    addLegal(dialog, app::playerComponents(), false);
    addLegal(dialog, app::gtkComponents(), false);
    addLegal(dialog, app::codecComponents(), false);
    addLegal(dialog, app::dataComponents(), true);

    adw_about_dialog_set_debug_info(dialog, formatsText(registry).c_str());
    adw_about_dialog_set_debug_info_filename(dialog, "xpcog-formats.txt");

    adw_dialog_present(about, parent);
}

}  // namespace xpcog::gtk
