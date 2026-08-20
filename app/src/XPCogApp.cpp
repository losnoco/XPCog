#include "XPCogApp.hpp"

#include "AppIcon.hpp"
#include "MainFrame.hpp"
#include "StatusPresence.hpp"
#include "Text.hpp"

#include "xpcog/platform/FileAssociations.hpp"
#include "xpcog/platform/SettingsStore.hpp"

#include <wx/cmdline.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/textfile.h>
#include <wx/image.h>
#include <wx/msgdlg.h>

#include <span>
#include <string>
#include <vector>

wxIMPLEMENT_APP(xpcog::app::XPCogApp);

namespace xpcog::app {
namespace {

const wxCmdLineEntryDesc kCommandLine[] = {
    {wxCMD_LINE_SWITCH, nullptr, "register",
     "offer XPCog for the audio file types this build understands", wxCMD_LINE_VAL_NONE,
     wxCMD_LINE_PARAM_OPTIONAL},
    {wxCMD_LINE_SWITCH, nullptr, "unregister", "undo --register", wxCMD_LINE_VAL_NONE,
     wxCMD_LINE_PARAM_OPTIONAL},
    {wxCMD_LINE_PARAM, nullptr, nullptr, "file", wxCMD_LINE_VAL_STRING,
     wxCMD_LINE_PARAM_OPTIONAL | wxCMD_LINE_PARAM_MULTIPLE},
    {wxCMD_LINE_NONE, nullptr, nullptr, nullptr, wxCMD_LINE_VAL_NONE, 0},
};

}  // namespace

void XPCogApp::OnInitCmdLine(wxCmdLineParser& parser) {
    parser.SetDesc(kCommandLine);
    // Off: a path beginning with a slash is a path, and on Windows `/` is also a
    // switch character by default. A player opens files far more often than it
    // takes options.
    parser.SetSwitchChars("-");
}

bool XPCogApp::OnCmdLineParsed(wxCmdLineParser& parser) {
    wantsRegister_   = parser.Found("register");
    wantsUnregister_ = parser.Found("unregister");
    for (std::size_t i = 0; i < parser.GetParamCount(); ++i) {
        pending_.Add(parser.GetParam(i));
    }
    return true;
}

bool XPCogApp::OnInit() {
    if (!wxApp::OnInit()) {
        return false;
    }

    SetVendorName("LoSnoCo");
    SetAppName("XPCog");
    SetAppDisplayName("XPCog");

    // Before the first decode. Without these a PNG loads as an empty bitmap and
    // reports nothing, which shows up as a missing icon rather than as an error.
    wxImage::AddHandler(new wxPNGHandler);
    wxImage::AddHandler(new wxJPEGHandler);

    // Registration is not playback: it runs before anything expensive is built,
    // and exits without ever opening a window.
    if (wantsRegister_ || wantsUnregister_) {
        performRegistration(wantsUnregister_);
        return finishEarly();
    }

    // Before anything expensive is constructed. A later launch's only job is to
    // hand its files over and go, and every decoder registered or database opened
    // first is latency the user waits through for a process that is about to
    // exit -- and, in the library's case, a second connection to a file the
    // running instance already holds.
    //
    // This existed for a whole commit before anything called it, which is the
    // failure a dead-code check would have caught and a test suite did not: the
    // class was complete, compiled, and unreachable, so every double-click opened
    // a second player.
#ifndef __WXOSX__
    instance_ = std::make_unique<SingleInstance>();
    {
        std::vector<std::string> arguments;
        arguments.reserve(pending_.GetCount());
        for (const wxString& name : pending_) {
            arguments.push_back(toUtf8(name));
        }
        if (!instance_->claim(arguments)) {
            // Handed over; there is nothing left for this process to do.
            return finishEarly();
        }
    }
#endif

    store_    = platform::makeNativeSettingsStore();
    settings_ = std::make_unique<Settings>(*store_);
    settings_->applyMigrations();

    // Settings before codecs are built from it: a decoder is handed these on
    // construction, so the registry has to be holding them by the time anything
    // asks it to open a file.
    registry_ = std::make_unique<PluginRegistry>();
    registry_->setSettings(settings_.get());
    registerAllCodecs(*registry_);

    frame_ = new MainFrame(*registry_, *settings_, dispatcher());
    frame_->Show();

    // A later launch: take its files and come to the front. Raising even when it
    // brought none is the point -- someone who runs the application again while
    // it is minimised is asking for the window, and a launch that appears to do
    // nothing reads as the program having failed to start.
#ifndef __WXOSX__
    launchSubscription_ =
        instance_->launched.connect([this](const std::vector<std::string>& received) {
            std::vector<Url> urls;
            urls.reserve(received.size());
            for (const std::string& argument : received) {
                urls.push_back(
                    Url::fromLocalPath(std::filesystem::path{toWx(argument).ToStdWstring()}));
            }
            if (!urls.empty()) {
                frame_->openUrls(urls);
            }
            raiseWindow(frame_);
        });
#endif

    if (!pending_.IsEmpty()) {
        std::vector<Url> urls;
        urls.reserve(pending_.GetCount());
        for (const wxString& name : pending_) {
            urls.push_back(Url::fromLocalPath(std::filesystem::path{name.ToStdWstring()}));
        }
        frame_->openUrls(urls);
        pending_.Clear();
    }

    return true;
}

int XPCogApp::OnExit() {
    // The handover channel goes first, and explicitly rather than by member
    // order. An IPC server is registered with the OS -- a DDE service on Windows,
    // a socket file elsewhere -- and wx requires it to be torn down while the
    // application is still alive to do the deregistering. Left to the member
    // destructor it runs after wxWidgets has finished shutting itself down, which
    // is a crash on exit rather than a leak.
    //
    // The subscription goes with it, before the signal it is attached to.
    launchSubscription_.reset();
    instance_.reset();

    // Then, in this order: the registry holds a pointer to the settings, and the
    // settings hold a reference to the store.
    registry_.reset();
    settings_.reset();
    store_.reset();
    return wxApp::OnExit();
}

#if wxDEBUG_LEVEL

void XPCogApp::OnAssertFailure(const wxChar* file, int line, const wxChar* function,
                               const wxChar* condition, const wxChar* message) {
    // Beside the executable rather than in a user directory: this is a developer
    // artefact, and a debug build is already only ever run out of its build tree.
    const wxFileName log(wxStandardPaths::Get().GetExecutablePath());
    const wxString   path = log.GetPath() + wxFileName::GetPathSeparator() +
                          "xpcog-assertions.log";

    if (FILE* out = wxFopen(path, "a")) {
        std::fprintf(out, "%s:%d in %s\n  condition: %s\n  message:   %s\n\n",
                     static_cast<const char*>(wxString(file).utf8_str()), line,
                     static_cast<const char*>(wxString(function).utf8_str()),
                     static_cast<const char*>(wxString(condition).utf8_str()),
                     static_cast<const char*>(wxString(message).utf8_str()));
        std::fclose(out);
    }

    // Then the default handler, which shows a dialog while there is still an
    // event loop to show it on and does nothing useful once there is not.
    wxApp::OnAssertFailure(file, line, function, condition, message);
}

#endif  // wxDEBUG_LEVEL

bool XPCogApp::finishEarly() {
    // Not `return false`, which is what this used to be.
    //
    // wx reads a false from OnInit as "initialisation failed" and exits the
    // process with -1, which arrives at a shell as 255. Both callers here have
    // *succeeded* -- one registered the file associations, the other handed its
    // arguments to the running player -- and reporting failure for that is wrong
    // in a way a script would notice and a person would not.
    //
    // So OnInit succeeds and OnRun declines to start a loop. The first attempt at
    // this asked CallAfter to end the loop on its first turn, and hung: CallAfter
    // queues the event and then wakes the loop, but during OnInit there is no
    // loop and no window to wake, so the wake-up went nowhere and the process sat
    // in GetMessage for ever with a pending event nobody would ever process.
    finishedEarly_ = true;
    return true;
}

int XPCogApp::OnRun() {
    // No window, nothing to wait for. Skipping the loop entirely is what makes
    // the handover fast -- it is the whole point of the path -- and OnExit still
    // runs, so the IPC channel is torn down properly.
    if (finishedEarly_) {
        return 0;
    }
    return wxApp::OnRun();
}

std::function<void(std::function<void()>)> XPCogApp::dispatcher() {
    return [](std::function<void()> action) {
        // On the application rather than on any window: a queued call that
        // outlives the window it was aimed at is a real case -- a scan finishing
        // as the user quits -- and wxApp is the handler with the longest life.
        wxTheApp->CallAfter(std::move(action));
    };
}

bool XPCogApp::performRegistration(bool unregister) {
    const auto complain = [](const wxString& text) {
        wxMessageBox(text, "XPCog", wxOK | wxICON_WARNING);
    };

    if (!platform::fileAssociationsSupported()) {
        complain("File associations are handled by the package on this platform, "
                 "not by the application.");
        return false;
    }

    std::string error;
    if (unregister) {
        if (!platform::unregisterFileAssociations(&error)) {
            complain("Could not remove the file associations: " + toWx(error));
            return false;
        }
        wxMessageBox("XPCog is no longer offered for audio files.", "XPCog",
                     wxOK | wxICON_INFORMATION);
        return true;
    }

    // The extension list comes from the codec registry, so it is exactly what
    // this build accepts. A list written out by hand here, or in an installer
    // script, would be wrong the first time a decoder was added and would stay
    // wrong quietly.
    PluginRegistry codecs;
    registerAllCodecs(codecs);

    const std::span<const std::string> extensions = codecs.allExtensions();
    if (!platform::registerFileAssociations(extensions, &error)) {
        complain("Could not register the file associations: " + toWx(error));
        return false;
    }

    // Explicit about what this did and did not do. Windows will not let an
    // application make itself the default handler, so promising that the next
    // double-click opens XPCog would be a promise the OS breaks.
    wxMessageBox(
        wxString::Format(
            "XPCog is now offered for %zu audio file types.\n\n"
            "Windows does not let an application make itself the default. To open "
            "these files by double-clicking, choose Open with " "→" " Choose another "
            "app " "→" " XPCog, and tick \"Always use this app\".",
            extensions.size()),
        "XPCog", wxOK | wxICON_INFORMATION);
    return true;
}

#ifdef __WXOSX__

void XPCogApp::MacReopenApp() {
    if (frame_ != nullptr) {
        frame_->Show();
        frame_->Raise();
    }
}

void XPCogApp::MacOpenFiles(const wxArrayString& fileNames) {
    if (frame_ == nullptr) {
        for (const wxString& name : fileNames) {
            pending_.Add(name);
        }
        return;
    }
    std::vector<Url> urls;
    urls.reserve(fileNames.GetCount());
    for (const wxString& name : fileNames) {
        urls.push_back(Url::fromLocalPath(std::filesystem::path{name.ToStdWstring()}));
    }
    frame_->openUrls(urls);
}

#endif

}  // namespace xpcog::app
