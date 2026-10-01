// The GTK frontend's suite that needs a screen, and the one that does not.
//
// The same bargain tests/app/test_guipanes.cpp makes: a widget that is the
// wrong size, a dialog that recurses, a painted pane that reads a lane the
// tick did not fill -- none of it exists until a real toolkit hands real
// windows real sizes, so this opens them. Under Xvfb where
// tests/CMakeLists.txt found it; it skips where there is no display at all.
//
// The cases that do not need a display -- the accelerator spellings, the
// playlist model's arithmetic -- live here too, because they need GTK's
// types, and a second binary for two cases would be one binary too many.
//
// Every case builds the toolkit itself, once per process: gtk_init_check()
// is the one call that says whether a display exists, and it is not undone.
// Pictures land in $XPCOG_GUI_CAPTURE when it names a directory, through
// GSK rather than ImageMagick: a widget is rendered to a texture by the
// window's own renderer, which needs no root-window grab and shows exactly
// what GTK would put on screen.

#include "Accelerators.hpp"
#include "Dialogs.hpp"
#include "Localization.hpp"
#include "Panes.hpp"
#include "PlaylistModel.hpp"
#include "PlaylistPane.hpp"
#include "PreferencesDialog.hpp"
#include "SeekBar.hpp"
#include "Session.hpp"
#include "Translations.hpp"
#include "Visualizers.hpp"
#ifdef XPCOG_HAVE_SC55_PANEL
#include "Sc55View.hpp"
#endif

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/AudioTap.hpp"
#include "xpcog/core/audio/OfflineOutput.hpp"
#include "xpcog/core/audio/Waveform.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

// glib-compile-resources writes a C header with no G_BEGIN_DECLS in it.
extern "C" {
#include "xpcog-resources.h"
}

#include <adwaita.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

/// The environment the toolkit reads, set before it is read. A static
/// initialiser rather than a fixture because GLib caches its answers: the
/// first g_get_user_cache_dir() is the one that sticks, and the translations
/// test writes a .mo there. GDK_BACKEND is pinned to X11 so a Wayland
/// desktop running this under Xvfb does not open the test windows on the
/// real screen -- the same trap docs/GTKPORT.md records for the wx suite.
const bool kEnvironment = [] {
    static const std::filesystem::path home =
        std::filesystem::temp_directory_path() / "xpcog-gtk-tests";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    setenv("XDG_CACHE_HOME", (home / "cache").string().c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home / "config").string().c_str(), 1);
    setenv("XDG_DATA_HOME", (home / "data").string().c_str(), 1);
    if (std::getenv("GDK_BACKEND") == nullptr) {
        setenv("GDK_BACKEND", "x11", 1);
    }
    // No accessibility bus either, unless asked for. A bare CI runner has no
    // org.a11y.Bus, and GTK says so with a warning the complaint check below
    // counts as a failure -- whichever case happens to look next takes the
    // blame. Nothing here tests the bus; the widgets' accessible names are set
    // either way.
    if (std::getenv("GTK_A11Y") == nullptr) {
        setenv("GTK_A11Y", "none", 1);
    }
    return true;
}();

/// What GTK and GLib complained about since the last look: every warning
/// and critical, which for a widget tree means an allocation under a
/// minimum ("attempt to underallocate"), a missing builder object, a
/// property that does not exist. Printed on the console as well, where they
/// went before, so a failing case still reads.
std::vector<std::string>& complaints() {
    static std::vector<std::string> list;
    return list;
}

std::vector<std::string> toolkitComplaints() {
    std::vector<std::string> taken;
    taken.swap(complaints());
    return taken;
}

GLogWriterOutput logWriter(GLogLevelFlags level, const GLogField* fields, gsize count, gpointer) {
    if ((level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) != 0) {
        for (gsize i = 0; i < count; ++i) {
            if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value != nullptr) {
                complaints().emplace_back(static_cast<const char*>(fields[i].value));
            }
        }
    }
    return g_log_writer_default(level, fields, count, nullptr);
}

/// Starts the toolkit once. False when there is no display to open, which
/// is a skip and not a failure.
bool toolkit() {
    static const bool started = [] {
        (void)kEnvironment;
        g_log_set_writer_func(logWriter, nullptr, nullptr);
        if (!gtk_init_check()) {
            return false;
        }
        adw_init();
        xpcog_register_resource();
        return true;
    }();
    return started;
}

/// Runs the main context until nothing is pending, then a few frames more:
/// GTK allocates from its frame clock, which schedules itself a frame later
/// rather than being pending now.
void settle(int frames = 5) {
    for (int i = 0; i < frames; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        g_usleep(20000);
    }
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

/// A top-level window around `child`, presented and settled.
class TestWindow {
public:
    TestWindow(GtkWidget* child, int width, int height) {
        // An AdwWindow, as the application's is: an AdwDialog presented over
        // a plain GtkWindow opens as a window of its own instead, and is then
        // not in this window's tree to be found.
        window_ = GTK_WINDOW(adw_window_new());
        gtk_window_set_default_size(window_, width, height);
        adw_window_set_content(ADW_WINDOW(window_), child);
        gtk_window_present(window_);
        settle();
    }
    ~TestWindow() {
        gtk_window_destroy(window_);
        settle(2);
    }
    TestWindow(const TestWindow&)            = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    [[nodiscard]] GtkWindow* window() const { return window_; }

private:
    GtkWindow* window_;
};

/// Every widget under `root` of type `type`, depth first.
void collect(GtkWidget* root, GType type, std::vector<GtkWidget*>& out) {
    if (g_type_is_a(G_OBJECT_TYPE(root), type)) {
        out.push_back(root);
    }
    for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
         child                = gtk_widget_get_next_sibling(child)) {
        collect(child, type, out);
    }
}

/// Renders `widget` as the window would and writes it as a PNG, when the
/// capture directory is set. The renderer is the window's own.
void capture(GtkWidget* widget, const std::string& name) {
    const char* dir = std::getenv("XPCOG_GUI_CAPTURE");
    if (dir == nullptr || *dir == '\0') {
        return;
    }
    GtkNative* native = gtk_widget_get_native(widget);
    REQUIRE(native != nullptr);
    GskRenderer* renderer = gtk_native_get_renderer(native);
    REQUIRE(renderer != nullptr);

    const int width  = gtk_widget_get_width(widget);
    const int height = gtk_widget_get_height(widget);
    REQUIRE(width > 0);
    REQUIRE(height > 0);

    // A GtkWidgetPaintable shows the widget's *last painted* frame, and
    // takes it from the frame clock after the next paint; fresh from
    // gtk_widget_paintable_new() it is empty. So a few frames first.
    GdkPaintable* paintable = gtk_widget_paintable_new(widget);
    settle(3);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    REQUIRE(node != nullptr);
    const graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width),
                                                        static_cast<float>(height));
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &viewport);
    REQUIRE(texture != nullptr);
    const std::string file = std::string(dir) + "/" + name + ".png";
    CHECK(gdk_texture_save_to_png(texture, file.c_str()));
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
}

xpcog::PluginRegistry& registry() {
    static xpcog::PluginRegistry instance;
    static const bool            once = [] {
        xpcog::registerAllCodecs(instance);
        return true;
    }();
    (void)once;
    return instance;
}

/// A session that plays into nothing, over a temporary data directory: what
/// the preferences dialog wants to read its accounts and devices from.
struct SessionHarness {
    SessionHarness()
        : store(xpcog::makeMemorySettingsStore()),
          settings(*store),
          data(std::filesystem::temp_directory_path() / "xpcog-gtk-tests" /
               ("session-" + std::to_string(counter++))),
          session(registry(), settings, &xpcog::gtk::postToMainContext, options()) {}

    xpcog::app::Session::Options options() const {
        xpcog::app::Session::Options o;
        o.dataDirectory  = data.string();
        o.cacheDirectory = (data / "cache").string();
        o.makeOutput     = [](xpcog::RingBuffer& ring) { return xpcog::makeOfflineOutput(ring, 8.0); };
        std::filesystem::create_directories(data);
        return o;
    }

    static inline int                             counter = 0;
    std::unique_ptr<xpcog::ISettingsStore>          store;
    xpcog::Settings                               settings;
    std::filesystem::path                         data;
    xpcog::app::Session                           session;
};

/// A playlist of `count` numbered tracks.
void fill(xpcog::Playlist& playlist, int count) {
    std::vector<xpcog::PlaylistEntry> entries;
    for (int i = 1; i <= count; ++i) {
        xpcog::PlaylistEntry entry;
        entry.url      = *xpcog::Url::parse("file:///music/" + std::to_string(i) + ".flac");
        entry.rawTitle = "Track " + std::to_string(i);
        entry.artist   = xpcog::SharedString{i % 2 == 0 ? "Even" : "Odd"};
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));
}

}  // namespace

// --- no display needed ---------------------------------------------------------------

TEST_CASE("the accelerator converter spells GTK's grammar", "[gtk][accelerators]") {
    using xpcog::gtk::gtkAccelerator;
    CHECK(gtkAccelerator("Ctrl+O") == "<Control>o");
    CHECK(gtkAccelerator("Ctrl+Shift+O") == "<Control><Shift>o");
    CHECK(gtkAccelerator("Ctrl+Shift+L") == "<Control><Shift>l");
    CHECK(gtkAccelerator("Ctrl+Alt+F") == "<Control><Alt>f");
    CHECK(gtkAccelerator("Del") == "Delete");
    CHECK(gtkAccelerator("Ctrl+Q") == "<Control>q");
    CHECK(gtkAccelerator("Space") == "Space");
    CHECK(gtkAccelerator("Ctrl+Right") == "<Control>Right");
    CHECK(gtkAccelerator("Ctrl+,") == "<Control>comma");
    CHECK(gtkAccelerator("") == "");

    // Every spelling the table carries parses, which is what GTK would
    // otherwise say on the console at startup and nowhere else.
    for (const xpcog::app::CommandId id : xpcog::app::allCommands()) {
        const std::string table(xpcog::app::commandAccelerator(id));
        if (table.empty()) {
            continue;
        }
        const std::string   spelled = gtkAccelerator(table);
        guint               key     = 0;
        GdkModifierType     mods    = static_cast<GdkModifierType>(0);
        INFO(table << " -> " << spelled);
        CHECK(gtk_accelerator_parse(spelled.c_str(), &key, &mods));
        CHECK(key != 0);
    }
}

TEST_CASE("the playlist model reports rebuilds and keeps its rows by track", "[gtk][playlist]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    xpcog::Playlist     playlist;
    xpcog::PlaylistView view{playlist};
    fill(playlist, 5);

    auto* model = xpcog::gtk::playlistModelNew(view);
    auto  owned = xpcog::gtk::GObjectPtr<XpcogPlaylistModel>::adopt(model);
    REQUIRE(g_list_model_get_n_items(G_LIST_MODEL(model)) == 5);

    struct Change {
        guint position, removed, added;
    };
    std::vector<Change> changes;
    const gulong        handler = xpcog::gtk::connect<void(GListModel*, guint, guint, guint)>(
        model, "items-changed", [&](GListModel*, guint position, guint removed, guint added) {
            changes.push_back({position, removed, added});
        });

    // The row objects carry the track: the same object comes back for the
    // same track after a rebuild, which is what keeps a selection.
    auto* before = XPCOG_PLAYLIST_ROW(g_list_model_get_item(G_LIST_MODEL(model), 2));
    const xpcog::TrackId third = xpcog::gtk::playlistRowTrack(before);
    CHECK(xpcog::gtk::playlistRowText(before, xpcog::PlaylistView::Column::Title) == "Track 3");
    g_object_unref(before);

    // A sort is a rebuild: one items-changed over the whole list.
    view.setSort(xpcog::PlaylistView::Column::Artist, true);
    REQUIRE(changes.size() == 1);
    CHECK(changes.back().position == 0);
    CHECK(changes.back().removed == 5);
    CHECK(changes.back().added == 5);

    bool found = false;
    for (guint i = 0; i < 5; ++i) {
        auto* row = XPCOG_PLAYLIST_ROW(g_list_model_get_item(G_LIST_MODEL(model), i));
        if (xpcog::gtk::playlistRowTrack(row) == third) {
            found = true;
            CHECK(xpcog::gtk::playlistRowIndex(row) == i);
        }
        g_object_unref(row);
    }
    CHECK(found);

    // A row change is not a rebuild: the row says so itself, the list does not.
    int rowChanged = 0;
    auto* row       = XPCOG_PLAYLIST_ROW(g_list_model_get_item(G_LIST_MODEL(model), 0));
    const gulong rowHandler = xpcog::gtk::connect<void(GObject*)>(
        row, "changed", [&](GObject*) { ++rowChanged; });
    view.rowChanged.publish(0);
    CHECK(rowChanged == 1);
    CHECK(changes.size() == 1);
    g_signal_handler_disconnect(row, rowHandler);
    g_object_unref(row);

    // A filter is a rebuild too, and the count follows it.
    view.setFilter("Even");
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(model)) == 2);
    CHECK(changes.size() == 2);
    CHECK(changes.back().removed == 5);
    CHECK(changes.back().added == 2);

    g_signal_handler_disconnect(model, handler);
}

// --- a display ------------------------------------------------------------------------

TEST_CASE("every interface file in the resource builds", "[gtk][ui]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    char** names = g_resources_enumerate_children("/co/losno/XPCog/ui", G_RESOURCE_LOOKUP_FLAGS_NONE,
                                                  nullptr);
    REQUIRE(names != nullptr);
    int built = 0;
    for (char** name = names; *name != nullptr; ++name) {
        const std::string path = std::string("/co/losno/XPCog/ui/") + *name;
        INFO(path);
        GtkBuilder* builder = gtk_builder_new_from_resource(path.c_str());
        REQUIRE(builder != nullptr);
        GSList* objects = gtk_builder_get_objects(builder);
        CHECK(objects != nullptr);
        g_slist_free(objects);
        g_object_unref(builder);
        ++built;
    }
    g_strfreev(names);
    CHECK(built >= 2);  // window.ui and mini.ui
}

TEST_CASE("the interface files speak the chosen language", "[gtk][ui][translations]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    // Spanish, through the same call the application makes: the .mo written
    // to the cache and the domain bound to it, then a .ui built afterwards.
    REQUIRE(xpcog::gtk::installTranslations("es") == "es");
    GtkBuilder* builder = gtk_builder_new_from_resource("/co/losno/XPCog/ui/window.ui");
    auto*       entry   = GTK_SEARCH_ENTRY(gtk_builder_get_object(builder, "filter_entry"));
    REQUIRE(entry != nullptr);
    char* text = nullptr;
    g_object_get(entry, "placeholder-text", &text, nullptr);
    CHECK(std::string(text != nullptr ? text : "") == "Filtrar");
    g_free(text);
    g_object_unref(builder);

    // And the C++ half agrees, which is the point of one catalogue.
    CHECK(xpcog::app::tr("Filter") == "Filtrar");

    // Back to English for the cases after this one.
    REQUIRE(xpcog::gtk::installTranslations("en").empty());
    CHECK(xpcog::app::tr("Filter") == "Filter");
}

TEST_CASE("the preferences dialog walks its pages", "[gtk][preferences]") {
    // One page at a time with the main context run between, which is what
    // caught the wx dialog re-wrapping itself until the stack ran out: a
    // layout handler that recurses only recurses on a real window.
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    SessionHarness h;

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    TestWindow window(content, 900, 700);

    auto dialog = std::make_unique<xpcog::gtk::PreferencesDialog>(h.session, /*hasTray=*/false);
    int  closed = 0;
    const xpcog::Subscription onClosed = dialog->closed.connect([&] { ++closed; });
    dialog->present(GTK_WIDGET(window.window()), xpcog::gtk::PreferencesPage::Playlist);
    settle();

    std::vector<GtkWidget*> dialogs;
    collect(GTK_WIDGET(window.window()), ADW_TYPE_DIALOG, dialogs);
    REQUIRE(dialogs.size() == 1);
    auto* prefs = ADW_DIALOG(dialogs.front());

    std::vector<GtkWidget*> stacks;
    collect(GTK_WIDGET(prefs), ADW_TYPE_VIEW_STACK, stacks);
    REQUIRE(stacks.size() == 1);
    auto* stack = ADW_VIEW_STACK(stacks.front());
    CHECK(g_strcmp0(adw_view_stack_get_visible_child_name(stack), "playlist") == 0);

    std::vector<GtkWidget*> pages;
    collect(GTK_WIDGET(prefs), ADW_TYPE_PREFERENCES_PAGE, pages);
    // The wx dialog's twelve panes, page for page.
    CHECK(pages.size() == 12);

    for (GtkWidget* page : pages) {
        const char* name = adw_preferences_page_get_name(ADW_PREFERENCES_PAGE(page));
        INFO(name);
        adw_view_stack_set_visible_child_name(stack, name);
        settle(3);
        CHECK(g_strcmp0(adw_view_stack_get_visible_child_name(stack), name) == 0);
        // A page that came up with nothing on it is a page whose rows were
        // never built, which is the failure a walk can see.
        std::vector<GtkWidget*> rows;
        collect(page, ADW_TYPE_PREFERENCES_GROUP, rows);
        CHECK(!rows.empty());
        capture(GTK_WIDGET(window.window()), std::string("preferences-") + name);
        CHECK(toolkitComplaints().empty());
    }

    // Search narrows the sidebar to the pages with a matching row, by the
    // row's words rather than the page's name: LRCLIB is a row on General.
    std::vector<GtkWidget*> sidebars;
    collect(GTK_WIDGET(prefs), ADW_TYPE_VIEW_SWITCHER_SIDEBAR, sidebars);
    REQUIRE(sidebars.size() == 1);
    GtkFilter* filter =
        adw_view_switcher_sidebar_get_filter(ADW_VIEW_SWITCHER_SIDEBAR(sidebars.front()));
    REQUIRE(filter != nullptr);
    std::vector<GtkWidget*> entries;
    collect(GTK_WIDGET(prefs), GTK_TYPE_SEARCH_ENTRY, entries);
    REQUIRE(!entries.empty());
    const auto matching = [&] {
        std::vector<std::string> names;
        GListModel* all = G_LIST_MODEL(adw_view_stack_get_pages(stack));
        for (guint i = 0; i < g_list_model_get_n_items(all); ++i) {
            auto* page = static_cast<AdwViewStackPage*>(g_list_model_get_item(all, i));
            if (gtk_filter_match(filter, page)) {
                names.emplace_back(adw_view_stack_page_get_name(page));
            }
            g_object_unref(page);
        }
        g_object_unref(all);
        return names;
    };
    CHECK(matching().size() == 12);
    gtk_editable_set_text(GTK_EDITABLE(entries.front()), "lrclib");
    CHECK(matching() == std::vector<std::string>{"general"});
    gtk_editable_set_text(GTK_EDITABLE(entries.front()), "no such words anywhere");
    CHECK(matching().empty());
    gtk_editable_set_text(GTK_EDITABLE(entries.front()), "");
    CHECK(matching().size() == 12);

    adw_dialog_close(prefs);
    settle();
    CHECK(closed == 1);
    dialog.reset();
    settle();
}

TEST_CASE("the info pane shows a track's cover and the words around it", "[gtk][info]") {
    // The cover is the one thing the pane decodes itself, and the bytes it
    // hands GdkTexture are a GBytes -- boxed, not a GObject. Released as a
    // GObject they crashed the player the first time a track with art was
    // shown, which nothing here had ever done.
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    SessionHarness h;
    xpcog::Library* library = h.session.library();
    REQUIRE(library != nullptr);

    // A 1x1 PNG, the smallest image GdkTexture will decode.
    static constexpr std::array<unsigned char, 67> kPng = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    xpcog::PlaylistEntry entry;
    entry.url      = *xpcog::Url::parse("file:///music/cover.flac");
    entry.rawTitle = "With a cover";
    entry.artist   = "Someone";
    entry.artHash  = library->storeArtwork(std::as_bytes(std::span(kPng)));

    xpcog::gtk::InfoPane pane(library);
    TestWindow           window(pane.widget(), 320, 480);
    settle();
    pane.showEntry(&entry);
    settle(3);
    std::vector<GtkWidget*> pictures;
    collect(pane.widget(), GTK_TYPE_PICTURE, pictures);
    REQUIRE(pictures.size() == 1);
    CHECK(gtk_widget_get_visible(pictures.front()));
    CHECK(gtk_picture_get_paintable(GTK_PICTURE(pictures.front())) != nullptr);
    // A 1x1 cover is still given room to be seen.
    CHECK(gtk_widget_get_height(pictures.front()) >= 180);

    // Clicking it shows the cover on its own, over the window, and a click on
    // that puts it away.
    std::vector<GtkWidget*> buttons;
    collect(pane.widget(), GTK_TYPE_BUTTON, buttons);
    REQUIRE(buttons.size() == 1);
    g_signal_emit_by_name(buttons.front(), "clicked");
    settle(3);
    std::vector<GtkWidget*> dialogs;
    collect(GTK_WIDGET(window.window()), ADW_TYPE_DIALOG, dialogs);
    REQUIRE(dialogs.size() == 1);
    // No album artist and no album on this entry, so the artist and the
    // track stand in.
    CHECK(g_strcmp0(adw_dialog_get_title(ADW_DIALOG(dialogs.front())),
                    "Someone \xE2\x80\x94 With a cover") == 0);
    std::vector<GtkWidget*> large;
    collect(dialogs.front(), GTK_TYPE_PICTURE, large);
    REQUIRE(large.size() == 1);
    CHECK(gtk_picture_get_paintable(GTK_PICTURE(large.front())) ==
          gtk_picture_get_paintable(GTK_PICTURE(pictures.front())));
    capture(GTK_WIDGET(window.window()), "info-artwork");
    // Closed by the click the picture carries, not by the test reaching in.
    int closed = 0;
    g_signal_connect_swapped(dialogs.front(), "closed",
                             G_CALLBACK(+[](int* count) { ++*count; }), &closed);
    auto* controllers = gtk_widget_observe_controllers(large.front());
    GtkGesture* click = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto* controller = static_cast<GObject*>(g_list_model_get_item(controllers, i));
        if (GTK_IS_GESTURE_CLICK(controller)) {
            click = GTK_GESTURE(controller);
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    REQUIRE(click != nullptr);
    g_signal_emit_by_name(click, "released", 1, 10.0, 10.0);
    settle(3);
    CHECK(closed == 1);

    // With both, the cover is named for the album.
    entry.albumArtist = "Various Artists";
    entry.album       = "The Compilation";
    pane.showEntry(&entry);
    settle();
    g_signal_emit_by_name(buttons.front(), "clicked");
    settle(3);
    dialogs.clear();
    collect(GTK_WIDGET(window.window()), ADW_TYPE_DIALOG, dialogs);
    REQUIRE(!dialogs.empty());
    CHECK(g_strcmp0(adw_dialog_get_title(ADW_DIALOG(dialogs.back())),
                    "Various Artists \xE2\x80\x94 The Compilation") == 0);
    adw_dialog_force_close(ADW_DIALOG(dialogs.back()));
    settle(3);

    // And again, and back to nothing: each redraw releases the last one's bytes.
    pane.showEntry(&entry);
    pane.showEntry(nullptr);
    settle();
    CHECK(toolkitComplaints().empty());
}

TEST_CASE("the lyrics pane offers its timed toggle only for timed words", "[gtk][lyrics]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    xpcog::gtk::LyricsPane pane([] { return 0.0; });
    TestWindow             window(pane.widget(), 320, 480);
    settle();

    std::vector<GtkWidget*> toggles;
    collect(pane.widget(), GTK_TYPE_TOGGLE_BUTTON, toggles);
    REQUIRE(toggles.size() == 1);
    GtkWidget* toggle = toggles.front();
    CHECK(g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(toggle)), "win.timed-lyrics") ==
          0);
    CHECK_FALSE(gtk_widget_get_visible(toggle));  // nothing on screen yet

    xpcog::PlaylistEntry entry;
    entry.url            = *xpcog::Url::parse("file:///music/timed.flac");
    entry.rawTitle       = "Timed";
    entry.unsyncedLyrics = "[00:01.00]first\n[00:03.00]second\n";
    pane.showEntry(&entry, false);
    settle();
    CHECK(gtk_widget_get_visible(toggle));

    // Turned off, the words are plain but still timeable, so the toggle
    // stays to turn timing back on.
    pane.setTimed(false);
    pane.showEntry(&entry, false);
    settle();
    CHECK(gtk_widget_get_visible(toggle));

    entry.unsyncedLyrics = "just words";
    pane.showEntry(&entry, false);
    settle();
    CHECK_FALSE(gtk_widget_get_visible(toggle));
    CHECK(toolkitComplaints().empty());
}

TEST_CASE("the shortcuts dialog is built from the command table", "[gtk][shortcuts]") {
    // The menu row it answers used to name win.show-help-overlay, an action
    // nothing installed, and sat greyed out. The dialog is ours now.
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    TestWindow window(content, 900, 700);
    settle();
    xpcog::gtk::showShortcutsDialog(GTK_WIDGET(window.window()));
    settle(3);

    std::vector<GtkWidget*> dialogs;
    collect(GTK_WIDGET(window.window()), ADW_TYPE_SHORTCUTS_DIALOG, dialogs);
    REQUIRE(dialogs.size() == 1);
    // Every menu with a shortcut, and General: File, Edit, Playback and View
    // all carry at least one in the table.
    std::vector<GtkWidget*> labels;
    collect(dialogs.front(), GTK_TYPE_LABEL, labels);
    const auto shows = [&](const char* text) {
        return std::any_of(labels.begin(), labels.end(), [text](GtkWidget* label) {
            return g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) == 0;
        });
    };
    CHECK(shows("Playback"));
    CHECK(shows("General"));
    CHECK(shows("Keyboard Shortcuts"));
    capture(GTK_WIDGET(window.window()), "shortcuts");
    adw_dialog_force_close(ADW_DIALOG(dialogs.front()));
    settle();
    CHECK(toolkitComplaints().empty());
}

TEST_CASE("the equaliser's scales are not narrower than they need", "[gtk][equalizer]") {
    // The WXPORT.md lesson: a control forced narrower than its own minimum is
    // one the toolkit refuses to draw, and only a laid-out window says so.
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    xpcog::gtk::ToolsStrip    strip;
    xpcog::gtk::EqualizerPane equalizer(settings);
    strip.addSection("equalizer", "Equalizer", equalizer.widget());
    strip.setShown("equalizer", true);
    TestWindow window(strip.widget(), 700, 260);
    settle(10);

    std::vector<GtkWidget*> scales;
    collect(equalizer.widget(), GTK_TYPE_SCALE, scales);
    // The preamp and the 31 bands.
    CHECK(scales.size() >= 32);
    // gtk_widget_get_width() is the content box, inside the CSS padding
    // Adwaita gives a scale, so it cannot be compared with the measured
    // minimum, which is outside it. What can be: the column each scale sits
    // in has at least the scale's minimum, and GTK itself said nothing --
    // an allocation under a minimum is a "attempt to underallocate" warning,
    // which the log hook below turns into a failure.
    for (GtkWidget* scale : scales) {
        GtkWidget* column = gtk_widget_get_parent(scale);
        REQUIRE(column != nullptr);
        int minimumWidth  = 0;
        int minimumHeight = 0;
        gtk_widget_measure(scale, GTK_ORIENTATION_HORIZONTAL, -1, &minimumWidth, nullptr, nullptr,
                           nullptr);
        gtk_widget_measure(scale, GTK_ORIENTATION_VERTICAL, -1, &minimumHeight, nullptr, nullptr,
                           nullptr);
        CHECK(gtk_widget_get_width(column) >= minimumWidth);
        CHECK(gtk_widget_get_height(column) >= minimumHeight);
        CHECK(minimumHeight >= 140);
    }
    CHECK(toolkitComplaints().empty());
    capture(strip.widget(), "equalizer");
}

TEST_CASE("the seek bar is GTK's slider plain and paints the waveform", "[gtk][seekbar]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    xpcog::gtk::SeekBar bar;
    GtkWidget*          box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), bar.widget());
    TestWindow window(box, 500, 120);

    // Plain, the stack shows the native slider, and playback moves it.
    std::vector<GtkWidget*> scales;
    collect(bar.widget(), GTK_TYPE_SCALE, scales);
    REQUIRE(scales.size() == 1);
    GtkWidget* scale = scales.front();
    std::vector<GtkWidget*> areas;
    collect(bar.widget(), GTK_TYPE_DRAWING_AREA, areas);
    REQUIRE(areas.size() == 1);
    GtkWidget* area = areas.front();

    // No length, nothing to seek within.
    CHECK_FALSE(gtk_widget_get_sensitive(scale));

    bar.setDuration(100.0);
    bar.setPosition(25.0);
    settle();
    CHECK(gtk_widget_get_sensitive(scale));
    CHECK(gtk_stack_get_visible_child(GTK_STACK(bar.widget())) == scale);
    CHECK(gtk_range_get_value(GTK_RANGE(scale)) == 25.0);
    CHECK(gtk_widget_get_width(bar.widget()) > 200);
    // The row is as tall as GTK's slider wants and no taller.
    int scaleHeight = 0;
    gtk_widget_measure(scale, GTK_ORIENTATION_VERTICAL, -1, &scaleHeight, nullptr, nullptr, nullptr);
    CHECK(gtk_widget_get_height(bar.widget()) == scaleHeight);
    capture(bar.widget(), "seekbar-plain");

    // A keyboard step has no press behind it, so it seeks at once -- and
    // does not count as a scrub that would freeze the position.
    std::vector<double> seeks;
    const xpcog::Subscription onSeek = bar.seekRequested.connect([&](double s) { seeks.push_back(s); });
    gboolean handled = FALSE;
    g_signal_emit_by_name(scale, "change-value", GTK_SCROLL_STEP_FORWARD, 30.0, &handled);
    REQUIRE(seeks.size() == 1);
    CHECK(seeks.front() == 30.0);
    CHECK_FALSE(bar.scrubbing());
    bar.setPosition(40.0);
    CHECK(gtk_range_get_value(GTK_RANGE(scale)) == 40.0);

    // The shape: a ramp, so the two levels and the two colours are visible.
    auto summary         = std::make_shared<xpcog::WaveformSummary>();
    summary->bucketCount = 400;
    summary->analysed    = 300;  // the last quarter still the plain groove
    for (int i = 0; i < 400; ++i) {
        summary->peak.push_back(static_cast<std::uint8_t>(40 + (i * 200) / 400));
        summary->rms.push_back(static_cast<std::uint8_t>(20 + (i * 120) / 400));
    }
    xpcog::gtk::SeekBar::WaveformStyle style;
    // Waveform mode before a shape is known keeps the slider, in the taller strip.
    bar.setWaveformMode(true);
    settle();
    CHECK(gtk_stack_get_visible_child(GTK_STACK(bar.widget())) == scale);
    CHECK(gtk_widget_get_height(bar.widget()) >= 28);
    for (const bool rectified : {false, true}) {
        for (const bool logarithmic : {false, true}) {
            style.rectified   = rectified;
            style.logarithmic = logarithmic;
            bar.setWaveformStyle(style);
            bar.setWaveformMode(true);
            bar.setWaveform(summary);
            settle(3);
            CHECK(gtk_stack_get_visible_child(GTK_STACK(bar.widget())) == area);
            CHECK(gtk_widget_get_height(bar.widget()) >= 28);
            capture(bar.widget(), std::string("seekbar-waveform-") +
                                      (rectified ? "rectified" : "mirrored") + "-" +
                                      (logarithmic ? "log" : "linear"));
        }
    }
    bar.setWaveformMode(false);
    settle();
    CHECK(gtk_stack_get_visible_child(GTK_STACK(bar.widget())) == scale);
    CHECK(gtk_widget_get_height(bar.widget()) == scaleHeight);
    CHECK(toolkitComplaints().empty());
}

TEST_CASE("the spectrum draws every channel mode and its menu writes settings",
          "[gtk][spectrum]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    // A second of stereo: a tone on the left, a different one on the right,
    // so the two analysers have different things to show.
    xpcog::AudioTap    tap;
    std::vector<float> stereo;
    for (int frame = 0; frame < 48000; ++frame) {
        const double t = frame / 48000.0;
        stereo.push_back(static_cast<float>(0.6 * std::sin(2.0 * 3.14159265 * 220.0 * t)));
        stereo.push_back(static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 660.0 * t)));
    }
    tap.write(stereo.data(), stereo.size(), 2);

    xpcog::gtk::SpectrumView spectrum(tap, settings);
    TestWindow               window(spectrum.widget(), 600, 200);
    spectrum.setSampleRate(48000.0);

    std::vector<std::string>  announced;
    const xpcog::Subscription onChange =
        spectrum.settingChanged.connect([&](const std::string& key) { announced.push_back(key); });

    for (const char* key : {"left", "right", "mirrored", "stacked", "overlaid", "mono"}) {
        // Through the menu's action, as a click on its row would.
        gtk_widget_activate_action(spectrum.widget(), "spectrum.channels", "s", key);
        CHECK(settings.SpectrumChannels() == key);
        // What the window does with the announcement.
        spectrum.applySettings(settings);
        CHECK(spectrum.channels() == xpcog::app::spectrumChannelsFromKey(key));

        for (const bool frequencies : {false, true}) {
            settings.setSpectrumFreqMode(frequencies);
            spectrum.applySettings(settings);
            spectrum.setActive(true);
            settle(6);
            capture(spectrum.widget(),
                    std::string("spectrum-") + key + "-" + (frequencies ? "freq" : "notes"));
            spectrum.setActive(false);
            settle(1);
        }
    }
    CHECK(announced.size() == 6);
    CHECK(announced.back() == "spectrumChannels");

    gtk_widget_activate_action(spectrum.widget(), "spectrum.peaks", nullptr);
    CHECK_FALSE(settings.SpectrumShowPeaks());
    gtk_widget_activate_action(spectrum.widget(), "spectrum.peaks", nullptr);
    CHECK(settings.SpectrumShowPeaks());
    CHECK(announced.size() == 8);

    int                       requested = 0;
    const xpcog::Subscription onRequest = spectrum.settingsRequested.connect([&] { ++requested; });
    gtk_widget_activate_action(spectrum.widget(), "spectrum.preferences", nullptr);
    CHECK(requested == 1);
    CHECK(announced.size() == 8);
    CHECK(toolkitComplaints().empty());
}

TEST_CASE("the oscilloscope draws every channel mode and its menu writes settings",
          "[gtk][scope]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);

    xpcog::AudioTap    tap;
    std::vector<float> stereo;
    for (int frame = 0; frame < 48000; ++frame) {
        const double t = frame / 48000.0;
        stereo.push_back(static_cast<float>(0.8 * std::sin(2.0 * 3.14159265 * 110.0 * t)));
        stereo.push_back(static_cast<float>(0.4 * std::sin(2.0 * 3.14159265 * 330.0 * t)));
    }
    tap.write(stereo.data(), stereo.size(), 2);

    xpcog::gtk::OscilloscopeView scope(tap, settings);
    TestWindow                   window(scope.widget(), 600, 200);
    scope.setSampleRate(48000.0);

    std::vector<std::string>  announced;
    const xpcog::Subscription onChange =
        scope.settingChanged.connect([&](const std::string& key) { announced.push_back(key); });

    for (const char* key : {"left", "right", "stacked", "overlaid", "mono"}) {
        gtk_widget_activate_action(scope.widget(), "scope.channels", "s", key);
        CHECK(settings.ScopeChannels() == key);
        scope.applySettings(settings);
        CHECK(scope.channels() == xpcog::app::scopeChannelsFromKey(key));
        for (const bool fill : {false, true}) {
            settings.setScopeFill(fill);
            scope.applySettings(settings);
            scope.setActive(true);
            settle(6);
            capture(scope.widget(), std::string("scope-") + key + (fill ? "-fill" : ""));
            scope.setActive(false);
            settle(1);
        }
    }
    CHECK(announced.size() == 5);

    for (const char* action : {"scope.trigger", "scope.fill", "scope.log-scale"}) {
        gtk_widget_activate_action(scope.widget(), action, nullptr);
    }
    CHECK_FALSE(settings.ScopeTrigger());
    CHECK_FALSE(settings.ScopeFill());  // the loop left it on; the toggle turned it off
    CHECK(settings.ScopeLogScale());
    CHECK(announced.size() == 8);
    CHECK(toolkitComplaints().empty());
}

#ifdef XPCOG_HAVE_SC55_PANEL
TEST_CASE("the SC-55 panel explains itself with nothing to show", "[gtk][sc55]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    xpcog::gtk::Sc55View panel([] { return 0.0; });
    TestWindow           window(panel.widget(), 600, 260);
    panel.setActive(true);
    settle(4);
    std::vector<GtkWidget*> labels;
    collect(panel.widget(), GTK_TYPE_LABEL, labels);
    REQUIRE(!labels.empty());
    CHECK(std::string(gtk_label_get_text(GTK_LABEL(labels.front()))).find("SC-55") !=
          std::string::npos);
    capture(panel.widget(), "sc55-idle");
    panel.setActive(false);
    CHECK(toolkitComplaints().empty());
}
#endif

TEST_CASE("the playlist pane lays out its columns and keeps a selection across a sort",
          "[gtk][playlist]") {
    if (!toolkit()) {
        SKIP("no display: GTK could not initialise");
    }
    auto            store = xpcog::makeMemorySettingsStore();
    xpcog::Settings settings(*store);
    xpcog::Playlist     playlist;
    xpcog::PlaylistView view{playlist};
    fill(playlist, 20);

    xpcog::gtk::PlaylistPane pane(view, settings);
    TestWindow               window(pane.widget(), 1000, 400);
    settle(5);

    const xpcog::TrackId seventh = view.trackAt(6);
    CHECK(pane.revealTrack(seventh));
    settle(2);
    REQUIRE(pane.selectedTracks().size() == 1);
    CHECK(pane.selectedTracks().front() == seventh);

    // A sort rebuilds the model; the selection is by track and survives it.
    view.setSort(xpcog::PlaylistView::Column::Title, false);
    settle(2);
    REQUIRE(pane.selectedTracks().size() == 1);
    CHECK(pane.selectedTracks().front() == seventh);

    // The columns are laid out inside the list, none of them zero.
    std::vector<GtkWidget*> views;
    collect(pane.widget(), GTK_TYPE_COLUMN_VIEW, views);
    REQUIRE(views.size() == 1);
    GListModel* columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(views.front()));
    const guint count   = g_list_model_get_n_items(columns);
    CHECK(count == 6);
    capture(pane.widget(), "playlist");
    CHECK(toolkitComplaints().empty());
}
