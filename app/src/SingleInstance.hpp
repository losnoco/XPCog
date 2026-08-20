// One XPCog per user, with later launches handing their files to the first.
//
// The reason is not tidiness, it is the audio device and the library. Two copies
// of a player fight over both: the second one opens the same SQLite library the
// first has open, and double-clicking a file in Explorer while something is
// already playing starts a second stream over the top of it rather than doing
// what the user meant, which is "play this".
//
// Cog gets this for free -- macOS launches one instance of an app bundle and
// sends the rest as -application:openFiles: events, so AppController never has to
// think about it. Nothing off macOS works that way: Explorer and every Linux file
// manager run the executable again, so the arbitration has to be ours.
//
// --- What wx does differently, and why this file is half the size ---------
//
// The Qt version was one QLocalServer doing both jobs: owning the name answered
// "am I first", and the same socket carried the handover. That is elegant and it
// meant hand-rolling the wire format -- a length prefix, an accumulating
// readyRead handler, and a workaround for Windows returning nothing from
// readAll() once the peer had gone.
//
// wx splits the two, and both halves are already written. wxSingleInstanceChecker
// answers "am I first" (a named mutex on Windows, a lock file on Unix). wxServer
// and wxClient carry the handover, and wxConnection::Execute() is a **framed,
// synchronous** message -- so the length prefix, the accumulator, the Windows
// workaround and the waitForDisconnected() dance all delete.
//
// `<wx/ipc.h>`, deliberately, and not `<wx/sckipc.h>`. The two differ by exactly
// one thing that matters: wx/ipc.h picks **DDE on Windows** and Unix domain
// sockets elsewhere, while sckipc is TCP on loopback everywhere. An earlier
// version of this file reached for sckipc to have one set of class names to
// reason about, and dismissed DDE as a Windows-only mechanism it had no reason to
// want. That was backwards. A listening TCP socket on Windows means the firewall
// asks the user to approve a *music player* wanting network access, which is
// alarming, unanswerable and entirely self-inflicted. DDE opens no socket.
//
// What does not change is the foreground handover. permitForegroundHandover()
// must still be called before Execute(), for exactly the reason
// platform/Foreground.hpp documents: the process that wants the foreground is the
// one already running, and the one entitled to it is the one about to exit.

#pragma once

#include "xpcog/core/Signal.hpp"

#include <memory>
#include <string>
#include <vector>

class wxSingleInstanceChecker;
// wxServer is a per-platform typedef -- wxDDEServer on Windows, wxTCPServer
// elsewhere -- so it cannot be forward-declared. Both derive from this.
class wxServerBase;

namespace xpcog::app {

class SingleInstance {
public:
    /// `name` overrides the identity, which otherwise means "XPCog, for this
    /// user". Tests must pass one: the default deliberately collides with a
    /// running player, so a test using it would claim nothing on a machine with
    /// XPCog open and pass or fail depending on what else is running.
    explicit SingleInstance(std::string name = {});
    ~SingleInstance();

    SingleInstance(const SingleInstance&)            = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// Tries to become *the* instance.
    ///
    /// Returns true when this process now owns the name and should carry on
    /// starting up. Returns false when another instance is already running, in
    /// which case `arguments` have been handed to it and this process should exit
    /// without opening a window.
    [[nodiscard]] bool claim(const std::vector<std::string>& arguments);

    /// A later launch arrived. The list is its file arguments, which may be
    /// empty -- someone launching the app again with no files is asking for the
    /// window they already have, so the receiver should still raise it.
    Signal<std::vector<std::string>> launched;

    /// The identity in use, so a test can address it.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// How the arguments cross the wire: one per line, UTF-8.
    ///
    /// A newline is unambiguous because a filename cannot contain one on Windows
    /// and is pathological if it does on Unix -- the same reasoning settings.def
    /// uses for the URL history. Exposed so the round trip can be tested without
    /// two processes.
    [[nodiscard]] static std::string encode(const std::vector<std::string>& arguments);
    [[nodiscard]] static std::vector<std::string> decode(const std::string& payload);

private:
    std::string name_;

    std::unique_ptr<wxSingleInstanceChecker> checker_;
    std::unique_ptr<wxServerBase>            server_;
};

}  // namespace xpcog::app
