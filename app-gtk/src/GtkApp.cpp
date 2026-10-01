#include "GtkApp.hpp"

#include "Accelerators.hpp"
#include "Localization.hpp"
#include "MainWindow.hpp"
#include "PlaybackController.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/platform/CrashReporter.hpp"
#include "xpcog/platform/DesktopIdentity.hpp"
#include "xpcog/platform/SettingsStore.hpp"

// glib-compile-resources writes a C header with no G_BEGIN_DECLS in it, so the
// linkage has to be said here or the linker looks for a mangled name.
extern "C" {
#include "xpcog-resources.h"
}

#include <cstdio>
#include <filesystem>

namespace xpcog::gtk {

namespace {

constexpr const char* kApplicationId = "co.losno.XPCog";

}  // namespace

GtkApp::GtkApp() {
    // Before the toolkit does anything, for the reason DesktopIdentity.hpp
    // gives: the program name is what a window's Wayland app_id and X11
    // WM_CLASS are taken from, and it is read once. GtkApplication also derives
    // an app_id from its application ID, so on Wayland this is belt and braces;
    // on X11 it is the only thing that fills in the WM_CLASS instance.
    platform::applyDesktopIdentity();

    // By hand, and before anything could ask for a resource: see
    // cmake/XPCogBlueprint.cmake for why the bundle does not register itself.
    xpcog_register_resource();

    // HANDLES_OPEN: files on the command line -- and from a second launch,
    // which GApplication forwards over D-Bus -- arrive on `open` rather than
    // being an error. Nothing else: the default flags already make the
    // application unique per session bus.
    app_ = GObjectPtr<AdwApplication>::adopt(
        adw_application_new(kApplicationId, G_APPLICATION_HANDLES_OPEN));

    // `--version`, because the wx build and the CLI both answer it and a
    // desktop file's Exec is not the only way this gets run.
    g_application_add_main_option(G_APPLICATION(app_.get()), "version", 'v',
                                  G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Print the version and exit", nullptr);

    auto* app = G_APPLICATION(app_.get());
    connect<void(GApplication*)>(app, "startup", [this](GApplication*) { onStartup(); });
    connect<void(GApplication*)>(app, "activate", [this](GApplication*) { onActivate(); });
    connect<void(GApplication*, GFile**, int, const char*)>(
        app, "open",
        [this](GApplication*, GFile** files, int count, const char*) { onOpen(files, count); });
    connect<void(GApplication*)>(app, "shutdown", [this](GApplication*) { onShutdown(); });
    connect<int(GApplication*, GVariantDict*)>(
        app, "handle-local-options",
        [this](GApplication*, GVariantDict* options) { return onHandleLocalOptions(options); });
}

GtkApp::~GtkApp() = default;

int GtkApp::run(int argc, char** argv) {
    return g_application_run(G_APPLICATION(app_.get()), argc, argv);
}

int GtkApp::onHandleLocalOptions(GVariantDict* options) {
    if (g_variant_dict_contains(options, "version")) {
        std::printf("%s\n", std::string(versionBanner()).c_str());
        return 0;  // handled: exit with this status, do not activate
    }
    return -1;  // carry on to startup and activate
}

void GtkApp::onStartup() {
    // The name the shell shows for the process where it has nothing better --
    // a notification's source, a portal dialog's "X wants to..." line.
    g_set_application_name("XPCog");

    store_    = platform::makeNativeSettingsStore();
    settings_ = std::make_unique<Settings>(*store_);
    settings_->applyMigrations();

    // Before the first window, and before anything that can put a message on
    // screen: a catalogue installed later would leave whatever had already been
    // built labelled in English.
    installTranslations(settings_->Language());

    // Crash reporting, if and only if the listener has already said yes. Here
    // rather than after the window, so that a crash while the codecs are
    // registered or the library is opened is still reported.
    if (settings_->SentryConsented()) {
        platform::startCrashReporting();
    }

    // Settings before codecs are built from it: a decoder is handed these on
    // construction, so the registry has to be holding them by the time anything
    // asks it to open a file.
    registry_ = std::make_unique<PluginRegistry>();
    registry_->setSettings(settings_.get());
    registerAllCodecs(*registry_);

    // Everything that is not a window. The dispatcher is the default main
    // context, which is GTK's; see Glib.hpp.
    session_ = std::make_unique<app::Session>(*registry_, *settings_, &postToMainContext);

    // The shortcuts, on the application so they reach the window's actions
    // from anywhere in it.
    installAccelerators(GTK_APPLICATION(app_.get()));
}

void GtkApp::ensureWindow() {
    if (window_) {
        return;
    }
    window_ = std::make_unique<MainWindow>(app_.get(), *session_);

    // The desktop integration wants a window to exist; on this platform it
    // wants no handle from it. Then the playlist, the resumed track and the
    // remote server, now that the window is listening.
    session_->attachDesktop(nullptr);
    session_->start();

    ticker_.start(app::PlaybackController::kTickIntervalMs, [this] { session_->tick(); });

    // After the window is up, and after this activation has presented it:
    // a dialog wants a parent that is on screen.
    g_idle_add_once([](gpointer data) { static_cast<GtkApp*>(data)->window_->askCrashReportingConsent(); },
                    this);
}

void GtkApp::onActivate() {
    ensureWindow();
    window_->present();
}

void GtkApp::onOpen(GFile** files, int count) {
    ensureWindow();

    std::vector<Url> urls;
    urls.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        GStr path(g_file_get_path(files[i]));
        if (path) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path{path.c_str()}));
        } else if (GStr uri(g_file_get_uri(files[i])); uri) {
            // A URL rather than a file: GApplication hands those over as
            // GFiles too, and Url::parse knows what to do with them.
            if (std::optional<Url> url = Url::parse(uri.c_str())) {
                urls.push_back(*url);
            }
        }
    }
    if (!urls.empty()) {
        window_->openUrls(urls);
    }
    // Raising even when nothing was brought is the point -- someone who runs
    // the application again while it is minimised is asking for the window.
    window_->present();
}

void GtkApp::onShutdown() {
    // The window's close-request already saved; this is for a quit that
    // bypassed it. save() is safe to call twice.
    ticker_.stop();
    if (session_) {
        session_->save();
    }
    window_.reset();
}

}  // namespace xpcog::gtk
