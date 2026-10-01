// The application object: an AdwApplication and what hangs off it.
//
// AdwApplication rather than GtkApplication because that is what initialises
// libadwaita -- its stylesheet, the style manager that follows the desktop's
// light and dark preference -- and because a GApplication is the answer to
// three things the wx frontend had to build for itself: a single instance
// (the application ID is a D-Bus name, and a second launch hands its files to
// the first and exits), the `open` signal those files arrive on, and the main
// loop platform/'s GDBus objects need iterated. See docs/GTKPORT.md.
//
// Not a GObject subclass. A subclass earns its boilerplate when something
// else needs to instantiate the type by name -- a template, a GtkBuilder file
// -- and nothing does here. The C++ object owns the C one and connects to its
// signals; that is the whole relationship.
//
// What it owns, in the order XPCogApp::OnInit builds the same things: the
// settings store and the settings, the plugin registry with every codec
// registered into it, the session, and the window. The session is built in
// `startup` and the window in the first `activate`, which is GApplication's
// own division: startup is once per process, activate is once per launch,
// and a second launch's activate arrives in the first process.

#pragma once

#include "Glib.hpp"
#include "Session.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <adwaita.h>

#include <memory>

namespace xpcog::gtk {

class MainWindow;

class GtkApp {
public:
    GtkApp();
    ~GtkApp();

    GtkApp(const GtkApp&) = delete;
    GtkApp& operator=(const GtkApp&) = delete;

    /// g_application_run: parses the command line, claims the application ID,
    /// runs the main loop until the last window closes. Returns the exit code.
    int run(int argc, char** argv);

    [[nodiscard]] AdwApplication* application() const { return app_.get(); }

private:
    void onStartup();
    void onActivate();
    void onOpen(GFile** files, int count);
    void onShutdown();
    int  onHandleLocalOptions(GVariantDict* options);

    /// The window, made on the first activate.
    void ensureWindow();

    GObjectPtr<AdwApplication> app_;

    // Declaration order is destruction order in reverse: the window reads the
    // session, the session reads the registry and the settings, the settings
    // read the store.
    std::unique_ptr<ISettingsStore> store_;
    std::unique_ptr<Settings>       settings_;
    std::unique_ptr<PluginRegistry> registry_;
    std::unique_ptr<app::Session>   session_;
    std::unique_ptr<MainWindow>     window_;

    /// The session's heartbeat; see PlaybackController::kTickIntervalMs.
    Timeout ticker_;
};

}  // namespace xpcog::gtk
