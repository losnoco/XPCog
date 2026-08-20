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

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"

#include <wx/app.h>
#include <wx/arrstr.h>

#include <functional>
#include <memory>

namespace xpcog::app {

class MainFrame;

class XPCogApp : public wxApp {
public:
    bool OnInit() override;
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
    bool MacReopenApp() override;

    /// Files opened from the Finder. XPCog never handled these under Qt; wx
    /// surfaces the event, so it costs five lines to gain.
    void MacOpenFiles(const wxArrayString& fileNames) override;
#endif

    /// Runs `action` on the interface's thread. Safe from any thread.
    [[nodiscard]] std::function<void(std::function<void()>)> dispatcher();

private:
    bool performRegistration(bool unregister);

    std::unique_ptr<ISettingsStore> store_;
    std::unique_ptr<Settings>       settings_;
    std::unique_ptr<PluginRegistry> registry_;

    MainFrame* frame_ = nullptr;

    /// Files named on the command line, kept until there is a window to open
    /// them in.
    wxArrayString pending_;

    bool wantsRegister_   = false;
    bool wantsUnregister_ = false;
};

}  // namespace xpcog::app

wxDECLARE_APP(xpcog::app::XPCogApp);
