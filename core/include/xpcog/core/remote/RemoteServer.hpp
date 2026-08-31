// The REST remote control: an HTTP server over the player.
//
// XPCog can already be driven from outside its window, but only through whatever
// the three desktop platforms agree on -- MPRIS, SMTC and MediaPlayer.framework
// between them offer play, pause, stop, next, previous, seek and, on MPRIS
// alone, raise, quit, volume and OpenUri. Nothing there can read a playlist,
// edit one, search it, or move an equaliser band. This can.
//
// --- The socket is not free, and it is answered twice -----------------------
//
// app/src/SingleInstance.hpp records a decision against this program owning a
// listening socket at all: on Windows the firewall asks the user to approve a
// *music player* wanting network access, which is "alarming, unanswerable and
// entirely self-inflicted". That argument has not gone away, so it is answered
// the way the crash reporter answers its own -- opt in twice. XPCOG_WITH_REST
// decides whether any of this is compiled; `remoteEnable` decides whether it
// ever listens, and it is false by default. The default bind is loopback, which
// raises no firewall prompt.
//
// The token is not part of that: it is required on every request regardless of
// where the connection came from. A loopback exemption would mean every process
// on the machine can drive the player, which is a different promise from the one
// the preferences pane makes.
//
// --- Why handle() is public -------------------------------------------------
//
// The whole request pipeline -- auth, routing, validation, dispatch,
// serialisation -- is behind handle(), which takes a struct and answers with a
// struct. cpp-httplib sits outside it and does nothing but convert. That is the
// seam IHttpClient is for the client side: it means the router, the OpenAPI
// document and every error shape are testable in xpcog-tests without binding a
// port, and the one test that does bind is testing the socket rather than the
// API.
//
// --- Threading --------------------------------------------------------------
//
// Requests arrive on cpp-httplib's threads. Almost nothing in this program may
// be touched from one: Playlist, PlaylistView, UndoStack, Library, Settings and
// Signal are all unlocked and single-threaded by convention. So every call into
// the player goes through a Dispatcher onto the interface thread and waits for
// the answer -- see CallGate, which is where the interesting parts of that are.
// GET /status is the exception and takes no hop at all: the interface pushes a
// snapshot in through publishStatus(), the way MainFrame already pushes
// setNowPlaying() into MediaIntegration.
//
// Nothing on the interface thread may call handle(). It would wait on itself.

#pragma once

#include "xpcog/core/Dispatcher.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog::remote {

class IPlayerControl;

struct ServerConfig {
    /// Loopback by default. A default, not a restriction -- the token is
    /// required either way, so binding wider is a choice the pane offers rather
    /// than a hole this class is guarding.
    std::string address = "127.0.0.1";
    int         port    = 7799;

    /// Required, and never empty: start() refuses without one rather than
    /// listening unauthenticated.
    std::string token;

    /// False serves the reads and refuses every write with 403. For a
    /// now-playing display that has no business editing anything.
    bool allowWrite = true;

    /// How long a request waits for the interface thread before answering 503.
    std::chrono::milliseconds callTimeout{2000};
};

/// A request with no HTTP library in it.
struct RawRequest {
    std::string method;
    std::string path;
    std::string query;   ///< Undecoded, without the '?'.
    std::string body;
    std::string contentType;
    std::string authorization;
    std::string peer;    ///< For rate limiting. Not logged.
};

struct RawResponse {
    int         status = 200;
    std::string body;
    std::string contentType = "application/json; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> headers;
};

class RemoteServer {
public:
    /// `control` must outlive this. `dispatch` must stay safe to call from any
    /// thread for as long as this lives, and must run its callable on the thread
    /// that owns the player.
    RemoteServer(IPlayerControl& control, Dispatcher dispatch, ServerConfig config);
    ~RemoteServer();

    RemoteServer(const RemoteServer&)            = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    /// Binds and starts serving. False with `error` filled in when it cannot --
    /// a port already taken is the ordinary case, and the preferences pane shows
    /// what it says.
    [[nodiscard]] bool start(std::string* error = nullptr);

    /// Releases every waiter, stops the listener and joins. Safe to call twice,
    /// and called by the destructor.
    ///
    /// The order matters and is a contract: this must finish before the
    /// IPlayerControl handed to the constructor is destroyed. MainFrame declares
    /// its server after the playback controller for exactly that reason.
    void stop();

    /// The port actually bound, which is what port 0 was asked for. Zero when
    /// not listening.
    [[nodiscard]] int boundPort() const;

    /// The whole request pipeline, with no socket involved.
    [[nodiscard]] RawResponse handle(const RawRequest& request);

    /// The generated OpenAPI document. Static because it describes the route
    /// table rather than any particular server, and a string rather than a JSON
    /// type because no public header here names the parser.
    [[nodiscard]] static std::string openApiDocument();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when this build has a real server to hand out.
///
/// Lets the preferences pane ask before constructing anything, which is what it
/// wants: a build without XPCOG_WITH_REST greys the pane and says why, the same
/// shape as the crash-reporting checkbox in a build without Sentry.
[[nodiscard]] bool remoteServerAvailable() noexcept;

}  // namespace xpcog::remote
