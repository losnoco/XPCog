#include "FileTreePane.hpp"

#include "Translations.hpp"

#include <adwaita.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace xpcog::gtk {

namespace {

constexpr const char* kAttributes =
    "standard::name,standard::display-name,standard::type,standard::is-hidden,"
    "standard::symbolic-icon";

/// The file behind a directory list's row.
GFile* fileOf(GFileInfo* info) {
    return G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
}

bool isDirectory(GFileInfo* info) {
    return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}

/// Folders first, then names the way a file manager orders them.
int compareInfos(gconstpointer a, gconstpointer b, gpointer) {
    auto* left  = G_FILE_INFO(const_cast<gpointer>(a));
    auto* right = G_FILE_INFO(const_cast<gpointer>(b));
    const bool leftDir  = isDirectory(left);
    const bool rightDir = isDirectory(right);
    if (leftDir != rightDir) {
        return leftDir ? -1 : 1;
    }
    GStr leftKey(g_utf8_collate_key_for_filename(g_file_info_get_display_name(left), -1));
    GStr rightKey(g_utf8_collate_key_for_filename(g_file_info_get_display_name(right), -1));
    return g_strcmp0(leftKey.c_str(), rightKey.c_str());
}

/// The leaf name, or the whole thing when there is no leaf -- a drive root,
/// where filename() is empty and the path itself is the name.
std::string leafName(const std::string& path) {
    const std::filesystem::path p{path};
    const std::string           leaf = p.filename().string();
    return leaf.empty() ? path : leaf;
}

}  // namespace

FileTreePane::FileTreePane(const PluginRegistry& registry, GtkWindow* parent)
    : registry_(registry), parent_(parent) {
    // The frame: a toolbar view with the root button as its header and the
    // list as its content, which is what a sidebar in a libadwaita window
    // looks like.
    root_ = adw_toolbar_view_new();
    gtk_widget_set_size_request(root_, 200, -1);

    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("folder-open-symbolic"));
    rootLabel_ = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(rootLabel_, PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(rootLabel_));
    rootButton_ = GTK_BUTTON(gtk_button_new());
    gtk_button_set_child(rootButton_, content);
    gtk_widget_set_tooltip_text(GTK_WIDGET(rootButton_),
                                app::tr("Choose the folder to browse").c_str());
    gtk_widget_add_css_class(GTK_WIDGET(rootButton_), "flat");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(rootButton_));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(root_), header);

    connections_.push_back(Connection::to<void(GtkButton*)>(
        rootButton_, "clicked", [this](GtkButton*) { chooseRootPath(); }));

    // The list. Its model is set by rebuild(), once there is a root.
    auto* factory = gtk_signal_list_item_factory_new();
    connections_.push_back(Connection::to<void(GtkSignalListItemFactory*, GObject*)>(
        factory, "setup", [](GtkSignalListItemFactory*, GObject* object) {
            auto*      item     = GTK_LIST_ITEM(object);
            GtkWidget* expander = gtk_tree_expander_new();
            GtkWidget* row      = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* icon     = gtk_image_new();
            GtkWidget* label    = gtk_label_new("");
            gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(row), icon);
            gtk_box_append(GTK_BOX(row), label);
            gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), row);
            gtk_list_item_set_child(item, expander);
        }));
    connections_.push_back(Connection::to<void(GtkSignalListItemFactory*, GObject*)>(
        factory, "bind", [](GtkSignalListItemFactory*, GObject* object) {
            auto* item     = GTK_LIST_ITEM(object);
            auto* expander = GTK_TREE_EXPANDER(gtk_list_item_get_child(item));
            auto* row      = GTK_TREE_LIST_ROW(gtk_list_item_get_item(item));
            gtk_tree_expander_set_list_row(expander, row);
            auto* info = G_FILE_INFO(gtk_tree_list_row_get_item(row));
            if (info == nullptr) {
                return;
            }
            GtkWidget* box   = gtk_tree_expander_get_child(expander);
            GtkWidget* icon  = gtk_widget_get_first_child(box);
            GtkWidget* label = gtk_widget_get_last_child(box);
            gtk_label_set_text(GTK_LABEL(label), g_file_info_get_display_name(info));
            if (GIcon* symbolic = g_file_info_get_symbolic_icon(info)) {
                gtk_image_set_from_gicon(GTK_IMAGE(icon), symbolic);
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                             isDirectory(info) ? "folder-symbolic"
                                                               : "audio-x-generic-symbolic");
            }
            g_object_unref(info);
        }));

    selection_ = GObjectPtr<GtkMultiSelection>::adopt(gtk_multi_selection_new(nullptr));
    list_ = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(selection_.get())), factory);
    gtk_widget_add_css_class(list_, "navigation-sidebar");

    connections_.push_back(Connection::to<void(GtkListView*, guint)>(
        list_, "activate", [this](GtkListView*, guint) {
            // Everything selected, which the activated row is part of: Enter
            // on a multiple selection adds all of it, as wx's tree does.
            std::vector<Url> urls;
            GtkBitset* set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_.get()));
            GtkBitsetIter iter;
            guint         position = 0;
            if (gtk_bitset_iter_init_first(&iter, set, &position)) {
                do {
                    auto row = GObjectPtr<GtkTreeListRow>::adopt(GTK_TREE_LIST_ROW(
                        g_list_model_get_item(G_LIST_MODEL(selection_.get()), position)));
                    if (!row) {
                        continue;
                    }
                    auto info = GObjectPtr<GFileInfo>::adopt(
                        G_FILE_INFO(gtk_tree_list_row_get_item(row.get())));
                    if (!info) {
                        continue;
                    }
                    GStr path(g_file_get_path(fileOf(info.get())));
                    if (path) {
                        urls.push_back(Url::fromLocalPath(std::filesystem::path{path.c_str()}));
                    }
                } while (gtk_bitset_iter_next(&iter, &position));
            }
            gtk_bitset_unref(set);
            if (!urls.empty()) {
                activated.publish(urls);
            }
        }));

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_);
    gtk_widget_set_vexpand(scroller, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(root_), scroller);
}

FileTreePane::~FileTreePane() = default;

bool FileTreePane::shows(GFileInfo* info) const {
    if (g_file_info_get_is_hidden(info)) {
        return false;
    }
    if (isDirectory(info)) {
        return true;
    }
    // The registry's extensions, lower-cased, against the name's.
    const std::string_view name = g_file_info_get_name(info);
    const std::size_t      dot  = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string extension(name.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string& known : registry_.allExtensions()) {
        if (known == extension) {
            return true;
        }
    }
    return false;
}

GListModel* FileTreePane::listFor(GFile* directory) const {
    auto* list = gtk_directory_list_new(kAttributes, directory);
    gtk_directory_list_set_monitored(list, TRUE);

    auto* filter = gtk_custom_filter_new(
        [](gpointer item, gpointer self) -> gboolean {
            return static_cast<const FileTreePane*>(self)->shows(G_FILE_INFO(item)) ? TRUE : FALSE;
        },
        const_cast<FileTreePane*>(this), nullptr);
    auto* filtered = gtk_filter_list_model_new(G_LIST_MODEL(list), GTK_FILTER(filter));

    auto* sorter = gtk_custom_sorter_new(compareInfos, nullptr, nullptr);
    auto* sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), GTK_SORTER(sorter));
    return G_LIST_MODEL(sorted);
}

void FileTreePane::rebuild() {
    if (rootPath_.empty()) {
        gtk_list_view_set_model(GTK_LIST_VIEW(list_), nullptr);
        gtk_label_set_text(rootLabel_, app::tr("Choose the folder to browse").c_str());
        return;
    }
    auto root = GObjectPtr<GFile>::adopt(g_file_new_for_path(rootPath_.c_str()));

    // Each folder's children are another list of the same shape; a file has
    // none, which is what null says.
    tree_ = GObjectPtr<GtkTreeListModel>::adopt(gtk_tree_list_model_new(
        listFor(root.get()), FALSE, FALSE,
        [](gpointer item, gpointer self) -> GListModel* {
            auto* info = G_FILE_INFO(item);
            if (!isDirectory(info)) {
                return nullptr;
            }
            return static_cast<FileTreePane*>(self)->listFor(fileOf(info));
        },
        this, nullptr));
    gtk_multi_selection_set_model(selection_.get(), G_LIST_MODEL(tree_.get()));
    gtk_list_view_set_model(GTK_LIST_VIEW(list_), GTK_SELECTION_MODEL(selection_.get()));
    gtk_label_set_text(rootLabel_, leafName(rootPath_).c_str());
}

void FileTreePane::setRootPath(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(std::filesystem::path{path}, error)) {
        // A root saved from a removable drive, or one since renamed. Keeping
        // whatever is currently shown beats emptying the tree with no
        // explanation.
        return;
    }
    rootPath_ = path;
    rebuild();
}

void FileTreePane::chooseRootPath() {
    auto dialog = GObjectPtr<GtkFileDialog>::adopt(gtk_file_dialog_new());
    gtk_file_dialog_set_title(dialog.get(), app::tr("Choose the folder to browse").c_str());
    if (!rootPath_.empty()) {
        auto initial = GObjectPtr<GFile>::adopt(g_file_new_for_path(rootPath_.c_str()));
        gtk_file_dialog_set_initial_folder(dialog.get(), initial.get());
    }
    gtk_file_dialog_select_folder(
        dialog.release(), parent_, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* self  = static_cast<FileTreePane*>(data);
            auto  owned = GObjectPtr<GtkFileDialog>::adopt(GTK_FILE_DIALOG(source));
            GErrorPtr error;
            auto folder = GObjectPtr<GFile>::adopt(
                gtk_file_dialog_select_folder_finish(owned.get(), result, &error.value));
            if (!folder) {
                return;
            }
            GStr path(g_file_get_path(folder.get()));
            if (path) {
                self->setRootPath(path.str());
                self->rootChosen.publish();
            }
        },
        this);
}

}  // namespace xpcog::gtk
