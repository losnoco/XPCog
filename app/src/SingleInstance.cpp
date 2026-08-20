#include "SingleInstance.hpp"

#include "Text.hpp"

#include "xpcog/platform/Foreground.hpp"

// sckipc rather than ipc: <wx/ipc.h> selects DDE on Windows and sockets
// everywhere else, so the class names differ per platform. Naming the socket
// implementation explicitly gives one set of types and one behaviour to reason
// about -- and DDE is a Windows-only mechanism this has no reason to want.
#include <wx/sckipc.h>
#include <wx/snglinst.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <cctype>
#include <utility>

namespace xpcog::app {
namespace {

/// The one topic this application speaks. wxIPC wants a service and a topic;
/// neither carries meaning here beyond "the same on both ends".
constexpr const char* kTopic = "open";

/// Per user, not per machine.
///
/// On Unix the lock file and the socket live in shared directories, so a bare
/// "XPCog" would mean the first user to log in owns the name and the second one's
/// player silently hands its files to a session it cannot see. Windows mutexes
/// are already session-scoped, but the name costs nothing there and keeping one
/// form avoids two behaviours to reason about.
[[nodiscard]] std::string defaultName() {
    std::string user = toUtf8(wxGetUserId());
    if (user.empty()) {
        user = "default";
    }
    // The name ends up as a path component on Unix and a mutex name on Windows,
    // and a Windows username can carry a domain separator. Keep the characters
    // both forms agree on rather than guessing which need escaping.
    std::string sanitised;
    sanitised.reserve(user.size());
    for (const char character : user) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            sanitised += character;
        }
    }
    return "XPCog-" + sanitised;
}

class HandoverConnection;

/// Set for the life of the server, so the connection can publish without every
/// wxIPC object having to carry a back pointer through wx's factory methods.
SingleInstance* g_owner = nullptr;

class HandoverConnection : public wxTCPConnection {
public:
    bool OnExecute(const wxString&, const void* data, std::size_t size,
                   wxIPCFormat) override {
        if (g_owner == nullptr || data == nullptr) {
            return false;
        }
        const std::string payload(static_cast<const char*>(data), size);
        g_owner->launched.publish(SingleInstance::decode(payload));
        return true;
    }
};

class HandoverServer : public wxTCPServer {
public:
    wxConnectionBase* OnAcceptConnection(const wxString& topic) override {
        return topic == kTopic ? new HandoverConnection : nullptr;
    }
};

class HandoverClient : public wxTCPClient {
public:
    wxConnectionBase* OnMakeConnection() override { return new HandoverConnection; }
};

/// Where the two ends meet. A path on Unix, which wx turns into a Unix domain
/// socket; a port number as a string on Windows, where wxIPC is TCP on loopback.
[[nodiscard]] wxString endpointFor(const std::string& name) {
#ifdef __WXMSW__
    // Derived from the name rather than fixed, so two users get two ports. The
    // range is the ephemeral one, and a collision is handled the same way a
    // stale name is: the connection is refused and this process claims the name.
    unsigned int hash = 2166136261U;
    for (const char character : name) {
        hash = (hash ^ static_cast<unsigned char>(character)) * 16777619U;
    }
    return wxString::Format("%u", 49152U + (hash % 8192U));
#else
    return toWx("/tmp/" + name + ".sock");
#endif
}

}  // namespace

SingleInstance::SingleInstance(std::string name)
    : name_(name.empty() ? defaultName() : std::move(name)) {}

SingleInstance::~SingleInstance() {
    if (g_owner == this) {
        g_owner = nullptr;
    }
}

std::string SingleInstance::encode(const std::vector<std::string>& arguments) {
    std::string payload;
    for (const std::string& argument : arguments) {
        if (!payload.empty()) {
            payload += '\n';
        }
        payload += argument;
    }
    return payload;
}

std::vector<std::string> SingleInstance::decode(const std::string& payload) {
    std::vector<std::string> arguments;
    std::size_t              start = 0;
    while (start <= payload.size()) {
        const std::size_t newline = payload.find('\n', start);
        const std::string line    = payload.substr(
            start, newline == std::string::npos ? std::string::npos : newline - start);
        if (!line.empty()) {
            arguments.push_back(line);
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return arguments;
}

bool SingleInstance::claim(const std::vector<std::string>& arguments) {
    checker_ = std::make_unique<wxSingleInstanceChecker>(toWx(name_));

    if (checker_->IsAnotherRunning()) {
        // Hand over and go. The permission has to be given *before* the message,
        // for the reason platform/Foreground.hpp documents at length: the process
        // that wants the foreground is the one already running, and the one
        // entitled to give it away is this one, which is about to exit.
        platform::permitForegroundHandover();

        HandoverClient client;
        const std::string payload = encode(arguments);

        // Execute() is framed and synchronous -- it returns after the server's
        // OnExecute has run -- which is the whole of what the Qt version's length
        // prefix, read accumulator and waitForDisconnected() were doing by hand.
        if (auto* connection = dynamic_cast<wxTCPConnection*>(
                client.MakeConnection("localhost", endpointFor(name_), kTopic))) {
            connection->Execute(payload.data(), payload.size(), wxIPC_PRIVATE);
            connection->Disconnect();
            return false;
        }

        // The checker says something is running but nothing answered. A crashed
        // process leaves its lock behind on Unix, so this is the stale case:
        // carry on as the owner rather than exiting into nothing, which would
        // make the application unlaunchable until the file was deleted by hand.
        checker_.reset();
    }

    g_owner = this;
    server_ = std::make_unique<HandoverServer>();
    if (!server_->Create(endpointFor(name_))) {
        // No handover channel. Not fatal: this process is still the instance, and
        // the only thing lost is later launches being able to reach it.
        server_.reset();
    }
    return true;
}

}  // namespace xpcog::app
