// The application object.
//
// Everything the program owns is constructed here and passed down by reference:
// the codec registry, the settings store, the settings facade. Cog reaches for
// singletons (kPersistentContainer, AudioPlayer's class methods, NSUserDefaults)
// from anywhere; keeping ownership visible at the top is what lets the same
// objects be swapped for test doubles below it.
//
// Two things this owns that the Qt entry point did not have to think about:
//
// **The dispatcher.** Everything below the interface -- the platform layer, the
// scan task, the playback controller -- takes a `std::function<void(std::function
// <void()>)>` rather than knowing what a toolkit is. Here it is
// wxEvtHandler::CallAfter, which is documented as safe to call from a worker
// thread and which deletes its pending events when the handler is destroyed. That
// is the same guarantee QObject gave, and it is why a queued call landing after a
// window closes is safe rather than a crash.
//
// **The image handlers.** wx decodes nothing it has not been told about. The
// artwork in the info panel and the PNG masters behind the application icon both
// need these registered before the first decode, and the failure mode without
// them is an empty bitmap and no error.

#pragma once

#include "SingleInstance.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/Signal.hpp"

#include <wx/app.h>
#include <wx/arrstr.h>

#include <functional>
#include <memory>

namespace xpcog::app {

class MainFrame;

class XPCogApp : public wxApp {
public:
    /// Runs before the toolkit is initialised, which is the only reason it is
    /// overridden: wxGTK calls gtk_init() inside the base class implementation,
    /// and the desktop identity has to be set before that happens. See
    /// xpcog/platform/DesktopIdentity.hpp for what is being set and why the
    /// timing is the whole of it.
    bool Initialize(int& argc, wxChar** argv) override;

    bool OnInit() override;
    int  OnRun() override;
    int  OnExit() override;

    /// Command-line handling. `--register` and `--unregister` are about this
    /// installation rather than about playback, so they run and exit without ever
    /// opening a window.
    void OnInitCmdLine(wxCmdLineParser& parser) override;
    bool OnCmdLineParsed(wxCmdLineParser& parser) override;

#ifdef __WXOSX__
    /// The Dock's "reopen" gesture -- clicking the icon when every window is
    /// closed. The real callback, where the Qt build watched for an application
    /// state change and had to filter out a Cmd-Tab false positive.
    ///
    /// void, not bool: wx/osx/app.h declares it `virtual void MacReopenApp()`.
    /// Written from memory as returning a handled flag, which is what the
    /// equivalent hook does in several other toolkits, and which no compiler on
    /// this machine could contradict.
    void MacReopenApp() override;

    /// Files opened from the Finder. XPCog never handled these under Qt; wx
    /// surfaces the event, so it costs five lines to gain.
    void MacOpenFiles(const wxArrayString& fileNames) override;
#endif

    /// Runs `action` on the interface's thread. Safe from any thread.
    [[nodiscard]] std::function<void(std::function<void()>)> dispatcher();

#if wxDEBUG_LEVEL
    /// Writes assertions somewhere they can be read.
    ///
    /// This is a WIN32_EXECUTABLE: it has no console, so the default handler's
    /// message goes nowhere anyone will see it. Worse, an assertion raised
    /// while the application is shutting down has no event loop left to show a
    /// dialog on, so it aborts instead -- which arrives as a bare exit code and
    /// looks like a crash with no cause attached. Appending to a file costs
    /// nothing and turns that into a sentence.
    void OnAssertFailure(const wxChar* file, int line, const wxChar* function,
                         const wxChar* condition, const wxChar* message) override;
#endif

private:
    bool performRegistration(bool unregister);

    /// Ends the process successfully, without a window.
    ///
    /// For the two paths that finish their work in OnInit: registering the
    /// file associations, and handing a later launch's arguments to the
    /// player already running. Always returns true; see the implementation
    /// for why that is not the same as `return false`.
    bool finishEarly();

    /// One player per user. Claimed before anything expensive is built, so a
    /// second launch hands its files over and exits without opening a
    /// database connection the running instance already holds.
    ///
    /// Absent on macOS, where LaunchServices delivers a second open as an
    /// event to the running application rather than starting a process --
    /// claiming a name there would add a failure mode to solve a problem the
    /// platform does not have.
    std::unique_ptr<SingleInstance> instance_;
    Subscription                    launchSubscription_;

    std::unique_ptr<ISettingsStore> store_;
    std::unique_ptr<Settings>       settings_;
    std::unique_ptr<PluginRegistry> registry_;

    MainFrame* frame_ = nullptr;

    /// Files named on the command line, kept until there is a window to open
    /// them in.
    wxArrayString pending_;

    bool wantsRegister_   = false;
    bool wantsUnregister_ = false;

    /// Set by finishEarly(). Read by OnRun, which then runs no event loop.
    bool finishedEarly_ = false;
};

}  // namespace xpcog::app

wxDECLARE_APP(xpcog::app::XPCogApp);
