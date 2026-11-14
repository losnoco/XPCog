#include "TagEditorDialog.hpp"

#include "Glib.hpp"
#include "Translations.hpp"

#include <adwaita.h>

#include <memory>
#include <string>
#include <vector>

namespace xpcog::gtk {

using app::fmt;
using app::tr;
using app::trn;

namespace {

std::string joinValues(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

/// The counterpart of joinValues for the one-line free-value editor: "a, b"
/// is two values, a lone empty entry is none. A value with a comma and a
/// space in it cannot survive this -- promoted rows get a real multi-value
/// editor eventually, and free rows are the coarse path.
std::vector<std::string> splitValues(const std::string& text) {
    std::vector<std::string> out;
    std::string_view         rest{text};
    while (true) {
        const std::size_t comma = rest.find(", ");
        if (std::string part{rest.substr(0, comma)}; !part.empty()) {
            out.push_back(std::move(part));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 2);
    }
    return out;
}

/// The dialog's whole world: the model it shows, the widgets that answer to
/// it, and the connections that keep them in step. Owned by the dialog via
/// g_object_set_data_full, so it outlives the stack frame that built them
/// and dies with the widget they hang on.
struct State {
    app::Session&        session;
    std::vector<TrackId> ids;
    app::TagEditorModel  model;

    GtkWidget*              dialog     = nullptr;
    GtkWidget*              saveButton = nullptr;
    GtkWidget*              freeGroup  = nullptr;  // rebuilt wholesale
    GtkWidget*              artStatus  = nullptr;
    GtkWidget*              artRemove  = nullptr;
    GtkWidget*              artKeep    = nullptr;
    std::vector<Connection> connections;

    /// One label per promoted row, for the notes that annotate it.
    struct FieldRow {
        app::TagEditorModel::Field field;
        GtkWidget*                 note;
    };
    std::vector<FieldRow> fieldRows;

    /// Rebuilt with the free rows: index i is the note under free row i.
    std::vector<GtkWidget*> freeNotes;

    State(app::Session& session_, std::vector<TrackId> ids_,
          std::vector<app::TagEditorSource> sources)
        : session(session_), ids(std::move(ids_)), model(std::move(sources)) {}

    // --- the notes that annotate rows ---------------------------------------

    void setFieldNote(app::TagEditorModel::Field field) {
        for (const FieldRow& row : fieldRows) {
            if (row.field != field) {
                continue;
            }
            std::string note;
            if (model.differs(field)) {
                note = tr("The selected files disagree here; what you type replaces "
                          "it in all of them.");
            }
            if (const std::string fold = model.fieldNote(field); !fold.empty()) {
                note = note.empty() ? fold : note + " " + fold;
            }
            gtk_label_set_text(GTK_LABEL(row.note), note.c_str());
            gtk_widget_set_visible(row.note, !note.empty());
        }
    }

    void refreshFreeNotes() {
        const std::vector<app::TagEditorModel::FreeRow>& rows = model.freeRows();
        for (std::size_t i = 0; i < freeNotes.size() && i < rows.size(); ++i) {
            const std::string note = model.freeRowNote(rows[i]);
            gtk_label_set_text(GTK_LABEL(freeNotes[i]), note.c_str());
            gtk_widget_set_visible(freeNotes[i], !note.empty());
        }
    }

    void refreshSave() { gtk_widget_set_sensitive(saveButton, model.changed()); }

    // --- free-form rows: rebuilt whenever their number moves ------------------

    void rebuildFreeRows() {
        if (freeGroup == nullptr) {
            return;
        }
        while (GtkWidget* child = gtk_widget_get_first_child(freeGroup)) {
            gtk_box_remove(GTK_BOX(freeGroup), child);
        }
        freeNotes.clear();
        std::size_t index = 0;
        for (const app::TagEditorModel::FreeRow& row : model.freeRows()) {
            buildFreeRow(index++, row);
        }
        refreshSave();
    }

    void buildFreeRow(std::size_t index, const app::TagEditorModel::FreeRow& row) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

        GtkWidget* line   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* key    = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(key), tr("Name").c_str());
        gtk_editable_set_text(GTK_EDITABLE(key), row.key.c_str());
        gtk_widget_set_hexpand(key, TRUE);
        GtkWidget* value = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(value), tr("Values, separated by commas").c_str());
        gtk_editable_set_text(GTK_EDITABLE(value), joinValues(row.values).c_str());
        gtk_widget_set_hexpand(value, TRUE);
        GtkWidget* remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_tooltip_text(remove, tr("Remove this tag").c_str());
        gtk_box_append(GTK_BOX(line), key);
        gtk_box_append(GTK_BOX(line), value);
        gtk_box_append(GTK_BOX(line), remove);

        GtkWidget* note = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
        gtk_widget_add_css_class(note, "dim-label");
        gtk_widget_set_margin_start(note, 12);

        gtk_box_append(GTK_BOX(box), line);
        gtk_box_append(GTK_BOX(box), note);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(freeGroup), box);
        freeNotes.push_back(note);

        // Text is set before connecting, so seeding a row does not count as
        // editing it. The index is captured, not a pointer into the model's
        // vector: adding or removing a row rebuilds this one from scratch.
        connections.push_back(Connection::to<void(GtkEntry*)>(
            key, "changed", [this, index](GtkEntry* entry) {
                model.setFreeKey(index, gtk_editable_get_text(GTK_EDITABLE(entry)));
                refreshFreeNotes();
                refreshSave();
            }));
        connections.push_back(Connection::to<void(GtkEntry*)>(
            value, "changed", [this, index](GtkEntry* entry) {
                model.setFreeValues(index,
                                    splitValues(gtk_editable_get_text(GTK_EDITABLE(entry))));
                refreshFreeNotes();
                refreshSave();
            }));
        connections.push_back(Connection::to<void(GtkButton*)>(
            remove, "clicked", [this, index](GtkButton*) {
                model.removeFreeRow(index);
                rebuildFreeRows();
            }));

        const std::string noteText = model.freeRowNote(row);
        gtk_label_set_text(GTK_LABEL(note), noteText.c_str());
        gtk_widget_set_visible(note, !noteText.empty());
    }

    // --- cover section --------------------------------------------------------

    void updateArt() {
        using ArtAction = app::TagEditorModel::ArtAction;
        switch (model.artAction()) {
            case ArtAction::Keep:
                gtk_label_set_text(GTK_LABEL(artStatus),
                                   !model.embeddedArt().empty()
                                       ? tr("The file carries its own cover image.").c_str()
                                       : (model.artFromFolder()
                                              ? tr("This album's picture comes from a file in "
                                                   "its folder; it is not stored in this file.")
                                                    .c_str()
                                              : tr("The file carries no cover image.").c_str()));
                break;
            case ArtAction::Remove:
                gtk_label_set_text(GTK_LABEL(artStatus),
                                   tr("The cover will be removed when you save.").c_str());
                break;
            case ArtAction::Replace:
                gtk_label_set_text(GTK_LABEL(artStatus),
                                   tr("The chosen image will replace the file's cover.").c_str());
                break;
        }
        gtk_widget_set_sensitive(artRemove,
                                 model.artAction() == ArtAction::Keep && !model.embeddedArt().empty());
        gtk_widget_set_visible(artKeep, model.artAction() != ArtAction::Keep);
        refreshSave();
    }

    void submit() {
        if (model.batch()) {
            session.editTags(ids, model.buildChanges(), app::Session::TagEditMode::merge);
        } else {
            session.editTags(ids, model.build(), app::Session::TagEditMode::replace);
        }
        adw_dialog_close(ADW_DIALOG(dialog));
    }
};

}  // namespace

void showTagEditorDialog(GtkWindow* parent, app::Session& session,
                         const std::vector<TrackId>& ids) {
    std::vector<app::TagEditorSource> sources = session.tagEditorSources(ids);
    if (sources.empty()) {
        return;
    }

    auto state = std::make_shared<State>(session, ids, std::move(sources));

    AdwDialog* dialog = adw_dialog_new();
    state->dialog     = GTK_WIDGET(dialog);
    adw_dialog_set_title(dialog, tr("Edit Tags").c_str());
    adw_dialog_set_content_width(dialog, 560);

    GtkWidget* view   = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    GtkWidget* cancel = gtk_button_new_with_label(tr("Cancel").c_str());
    GtkWidget* save   = gtk_button_new_with_label(tr("Save").c_str());
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_sensitive(save, FALSE);
    state->saveButton = save;
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), save);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), cancel);

    GtkWidget* page = adw_preferences_page_new();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);

    // What the dialog is about: one file's name, or how many the batch edits.
    {
        GtkWidget* group = adw_preferences_group_new();
        if (state->model.batch()) {
            adw_preferences_group_set_description(
                ADW_PREFERENCES_GROUP(group),
                fmt(trn("Editing %zu file. Fields you change here are written to every one of "
                        "them; the rest stays as it is.",
                        "Editing %zu files. Fields you change here are written to every one of "
                        "them; the rest stays as it is.",
                        state->model.sources().size()),
                    state->model.sources().size())
                    .c_str());
        } else {
            adw_preferences_group_set_description(
                ADW_PREFERENCES_GROUP(group),
                state->model.sources().front().entry.filename().c_str());
        }
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
    }

    // --- the promoted fields ----------------------------------------------------

    {
        GtkWidget* group = adw_preferences_group_new();
        for (const app::TagEditorModel::Field field : app::TagEditorModel::kFields) {
            GtkWidget* row = adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          app::TagEditorModel::label(field).c_str());
            gtk_editable_set_text(GTK_EDITABLE(row), state->model.joined(field).c_str());
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

            GtkWidget* note = gtk_label_new("");
            gtk_label_set_wrap(GTK_LABEL(note), TRUE);
            gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
            gtk_widget_add_css_class(note, "dim-label");
            gtk_widget_set_margin_start(note, 12);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), note);
            state->fieldRows.push_back({field, note});

            // The delegate's text widget, not the row: AdwEntryRow has no
            // "changed" of its own, and the text lives in the delegate.
            // Connected after the initial text is set, so seeding does not
            // mark the row touched.
            GtkWidget* delegate = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(row)));
            state->connections.push_back(Connection::to<void(GtkEditable*)>(
                delegate, "changed", [state, field](GtkEditable* text) {
                    state->model.setJoined(field, gtk_editable_get_text(text));
                    state->setFieldNote(field);
                    state->refreshSave();
                }));
            state->setFieldNote(field);
        }
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
    }

    // --- the file's other tags: single file only ---------------------------------

    if (state->model.supportsFreeRows()) {
        GtkWidget* group = adw_preferences_group_new();
        state->freeGroup = group;
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), tr("Other tags").c_str());
        GtkWidget* add = gtk_button_new_with_label(tr("Add another tag").c_str());
        gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
        adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), add);
        state->connections.push_back(Connection::to<void(GtkButton*)>(
            add, "clicked", [state](GtkButton*) {
                state->model.addFreeRow();
                state->rebuildFreeRows();
            }));
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
        state->rebuildFreeRows();
    }

    // --- the cover: single file, on formats that keep pictures -------------------

    if (state->model.supportsArt()) {
        GtkWidget* group = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), tr("Cover").c_str());

        GtkWidget* status = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(status), TRUE);
        gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), status);
        state->artStatus = status;

        GtkWidget* line    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        state->artRemove   = gtk_button_new_with_label(tr("Remove").c_str());
        state->artKeep     = gtk_button_new_with_label(tr("Keep").c_str());
        GtkWidget* replace = gtk_button_new_with_label(tr("Replace...").c_str());
        gtk_box_append(GTK_BOX(line), state->artRemove);
        gtk_box_append(GTK_BOX(line), state->artKeep);
        gtk_box_append(GTK_BOX(line), replace);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), line);

        state->connections.push_back(Connection::to<void(GtkButton*)>(
            state->artRemove, "clicked", [state](GtkButton*) {
                state->model.removeArt();
                state->updateArt();
            }));
        state->connections.push_back(Connection::to<void(GtkButton*)>(
            state->artKeep, "clicked", [state](GtkButton*) {
                state->model.keepArt();
                state->updateArt();
            }));

        // The picker belongs to the window, not to the editor widget: the
        // editor stays up under it. Its callback owns a reference to the
        // state, because unlike a signal handler the callback can run after
        // any single widget the picker was built from is gone.
        state->connections.push_back(Connection::to<void(GtkButton*)>(
            replace, "clicked", [state, window = GTK_WIDGET(parent)](GtkButton*) {
                auto picker = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
                gtk_file_dialog_set_title(picker.get(), tr("Choose a cover image").c_str());
                auto images = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
                gtk_file_filter_set_name(images.get(), tr("Images").c_str());
                gtk_file_filter_add_suffix(images.get(), "png");
                gtk_file_filter_add_suffix(images.get(), "jpg");
                gtk_file_filter_add_suffix(images.get(), "jpeg");
                auto all = GObjectPtr<GtkFileFilter>::adopt(gtk_file_filter_new());
                gtk_file_filter_set_name(all.get(), tr("All Files").c_str());
                gtk_file_filter_add_pattern(all.get(), "*");
                auto filters = GObjectPtr<GListStore>::adopt(g_list_store_new(GTK_TYPE_FILE_FILTER));
                g_list_store_append(filters.get(), images.get());
                g_list_store_append(filters.get(), all.get());
                gtk_file_dialog_set_filters(picker.get(), G_LIST_MODEL(filters.get()));
                gtk_file_dialog_set_default_filter(picker.get(), images.get());
                gtk_file_dialog_open(
                    picker.release(), GTK_WINDOW(window), nullptr,
                    [](GObject* source, GAsyncResult* result, gpointer data) {
                        std::unique_ptr<std::shared_ptr<State>> owned{
                            static_cast<std::shared_ptr<State>*>(data)};
                        State& st = **owned;
                        GErrorPtr error;
                        auto file = GObjectPtr<GFile>::adopt(gtk_file_dialog_open_finish(
                            GTK_FILE_DIALOG(source), result, &error.value));
                        if (!file) {
                            return;
                        }
                        // Read on the UI thread: the file is a local image the
                        // picker just named, and the callback wants the bytes
                        // before it returns the model to the dialog.
                        char* contents = nullptr;
                        gsize length   = 0;
                        if (g_file_load_contents(file.get(), nullptr, &contents, &length, nullptr,
                                                 nullptr)) {
                            std::vector<std::byte> bytes;
                            bytes.assign(reinterpret_cast<const std::byte*>(contents),
                                         reinterpret_cast<const std::byte*>(contents) + length);
                            g_free(contents);
                            st.model.replaceArt(std::move(bytes));
                            st.updateArt();
                        }
                    },
                    new std::shared_ptr<State>(state));
            }));

        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
        state->updateArt();
    }

    state->connections.push_back(Connection::to<void(GtkButton*)>(
        cancel, "clicked", [dialog](GtkButton*) { adw_dialog_close(ADW_DIALOG(dialog)); }));
    state->connections.push_back(
        Connection::to<void(GtkButton*)>(save, "clicked", [state](GtkButton*) {
            state->submit();
        }));

    g_object_set_data_full(
        G_OBJECT(dialog), "xpcog-state", new std::shared_ptr<State>(state),
        [](gpointer data) { delete static_cast<std::shared_ptr<State>*>(data); });

    adw_dialog_present(dialog, GTK_WIDGET(parent));
}

}  // namespace xpcog::gtk
