#include "xpcog/core/remote/RemoteServer.hpp"

#include "CallGate.hpp"
#include "RateLimit.hpp"
#include "Router.hpp"
#include "Routes.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/core/remote/Token.hpp"

// Included here and nowhere a public header can see it. httplib.h pulls in
// <winsock2.h> on Windows, and a core header dragging that into every
// translation unit that includes it would be a fresh category of problem -- so
// the server lives behind Impl and the include stays in this file.
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace xpcog::remote {

// The shared response shapes live here rather than in Routes.cpp because the
// router needs them for the answers no handler ever produces -- 401, 404, 405,
// 413 -- and a route that failed validation and a request that never reached one
// should not answer in two different shapes.
RawResponse jsonResponse(const nlohmann::json& body, int status) {
    RawResponse response;
    response.status = status;
    response.body   = body.dump();
    return response;
}

RawResponse jsonError(int status, std::string_view code, std::string_view message,
                      std::string_view field) {
    nlohmann::json body;
    body["error"]["code"]    = code;
    body["error"]["message"] = message;
    if (!field.empty()) {
        body["error"]["field"] = field;
    }
    return jsonResponse(body, status);
}

RawResponse interfaceBusy() {
    RawResponse response =
        jsonError(503, "ui_busy", "The player did not answer in time.");
    response.headers.emplace_back("Retry-After", "1");
    return response;
}

RawResponse outcomeResponse(Outcome outcome, const nlohmann::json& body) {
    switch (outcome) {
        case Outcome::Ok:
            return jsonResponse(body.is_null() ? nlohmann::json::object() : body);
        case Outcome::Busy: {
            // A start or stop is in flight and the player would silently decline.
            // Saying so is the difference between an API that reports what
            // happened and one that reports what was asked.
            RawResponse response = jsonError(
                409, "busy", "A track is starting or stopping; try again shortly.");
            response.headers.emplace_back("Retry-After", "1");
            return response;
        }
        case Outcome::NotFound:
            return jsonError(404, "not_found", "No such track, key or preset.");
        case Outcome::Rejected:
            return jsonError(400, "rejected", "The player refused that value.");
        case Outcome::ReadOnly:
            return jsonError(403, "read_only",
                             "That is session state rather than a setting.");
        case Outcome::Unsupported:
            return jsonError(501, "unsupported", "This host cannot do that.");
    }
    return jsonError(500, "internal", "Unreachable.");
}

namespace {

/// The bearer token in an Authorization header, or empty when there is not one
/// in the shape this accepts.
///
/// The scheme is compared case-insensitively because RFC 7235 says it is
/// case-insensitive, and a client that sends "bearer" is not wrong.
std::string_view bearerToken(std::string_view authorization) {
    constexpr std::string_view kScheme = "bearer ";
    if (authorization.size() <= kScheme.size()) {
        return {};
    }
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        const char given = static_cast<char>(
            std::tolower(static_cast<unsigned char>(authorization[i])));
        if (given != kScheme[i]) {
            return {};
        }
    }
    std::string_view token = authorization.substr(kScheme.size());
    while (!token.empty() && token.front() == ' ') {
        token.remove_prefix(1);
    }
    return token;
}

/// The one answer for every way a request can fail to authenticate.
///
/// Missing, malformed and wrong are the same response down to the byte. A client
/// that could tell them apart could learn whether a token exists, whether it is
/// the right length, and eventually more; and none of that is worth the
/// diagnostic. The realm names the player so a browser's prompt says who is
/// asking.
RawResponse unauthorized() {
    RawResponse response = jsonError(401, "unauthorized", "A bearer token is required.");
    response.headers.emplace_back("WWW-Authenticate",
                                  R"(Bearer realm="XPCog", error="invalid_token")");
    return response;
}

}  // namespace

struct RemoteServer::Impl {
    IPlayerControl& control;
    Dispatcher      dispatch;
    ServerConfig    config;

    RateLimit       rateLimit;
    Router          router;
    CallGate        gate;
    httplib::Server server;
    std::thread     thread;
    int             port = 0;

    Impl(IPlayerControl& c, Dispatcher d, ServerConfig cfg)
        : control(c),
          dispatch(std::move(d)),
          config(std::move(cfg)),
          gate(control, dispatch, config.callTimeout) {}
};

RemoteServer::RemoteServer(IPlayerControl& control, Dispatcher dispatch,
                           ServerConfig config)
    : impl_(std::make_unique<Impl>(control, std::move(dispatch), std::move(config))) {}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::start(std::string* error) {
    // Refused rather than served open. A server with no token is not a
    // convenience, it is every process on the network holding the transport.
    if (impl_->config.token.empty()) {
        if (error != nullptr) {
            *error = "no access token";
        }
        return false;
    }

    // One handler per method, each matching every path, because routing is
    // handle()'s job and cpp-httplib's own router would be a second table to
    // keep in step with the one the OpenAPI document is generated from.
    //
    // Registered as route handlers rather than through
    // set_pre_routing_handler(), and that is not a style choice: httplib reads
    // the request body only *after* a matcher has matched, so a pre-routing
    // handler sees an empty body on every POST, PUT and PATCH. It cost a
    // socket test to notice, because handle() is fed a body directly everywhere
    // else and could not have shown it.
    auto serve = [this](const httplib::Request& in, httplib::Response& out) {
        RawRequest request;
        request.method = in.method;
        request.path   = in.path;
        // in.path is already decoded and stripped of the query; the raw target
        // is where the query still is.
        const std::size_t mark = in.target.find('?');
        request.query = (mark == std::string::npos) ? std::string{}
                                                    : in.target.substr(mark + 1);
        request.body          = in.body;
        request.contentType   = in.get_header_value("Content-Type");
        request.authorization  = in.get_header_value("Authorization");
        request.acceptEncoding = in.get_header_value("Accept-Encoding");
        request.peer          = in.remote_addr;

        const RawResponse answer = handle(request);
        for (const auto& [name, value] : answer.headers) {
            out.set_header(name, value);
        }
        out.status = answer.status;
        out.set_content(answer.body, answer.contentType);
    };

    impl_->server.Get(".*", serve);
    impl_->server.Post(".*", serve);
    impl_->server.Put(".*", serve);
    impl_->server.Patch(".*", serve);
    impl_->server.Delete(".*", serve);

    // One socket option, spelled differently on either side, and *not* httplib's
    // default on either.
    //
    // What is wanted is the same in both places: one XPCog owns the port, and a
    // second one -- or a stale one that has not exited -- is told the port is
    // taken rather than quietly sharing it. Sharing is the dangerous outcome,
    // because the kernel then hands each request to whichever process it likes
    // and a remote control drives an arbitrary one of two players; it also means
    // a clash is never reported, so the preferences pane could never say so.
    //
    // httplib's default_socket_options sets SO_REUSEPORT where the platform has
    // it, which is exactly that sharing. Right for a web server behind a load
    // balancer, wrong here.
    //
    // The names then invert. On POSIX, SO_REUSEADDR permits rebinding a port
    // whose previous socket is in TIME_WAIT -- what makes a port change in the
    // preferences pane take effect without a wait -- and still refuses a second
    // live listener. On Windows the same constant means something else entirely:
    // it *allows* binding a port another socket is actively listening on, which
    // is the hijack the POSIX option prevents. SO_EXCLUSIVEADDRUSE is the one
    // that means there what SO_REUSEADDR means everywhere else.
    //
    // Found by the socket test, on CI, having shipped in 1.5.0 -- there is no
    // Windows here to have caught it earlier.
    impl_->server.set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });

    // Bounded on purpose. cpp-httplib is a thread per connection, so without a
    // cap two clients holding keep-alive open can starve everything else.
    impl_->server.new_task_queue = [] { return new httplib::ThreadPool(4); };
    impl_->server.set_keep_alive_max_count(8);
    impl_->server.set_read_timeout(5, 0);
    // A body cap, or POST /playlist/tracks with a hundred thousand URLs is an
    // out-of-memory rather than a 413: httplib buffers a whole body before any
    // of this sees it.
    impl_->server.set_payload_max_length(1024U * 1024U);

    // Port 0 means "any free one", and the two calls are genuinely different:
    // bind_to_port answers a bool, so it cannot say which port it got, and only
    // bind_to_any_port returns one. Tests ask for 0 so they never collide with
    // whatever else is listening on the machine running them.
    if (impl_->config.port == 0) {
        impl_->port = impl_->server.bind_to_any_port(impl_->config.address);
        if (impl_->port <= 0) {
            if (error != nullptr) {
                *error = "could not bind " + impl_->config.address;
            }
            return false;
        }
    } else if (impl_->server.bind_to_port(impl_->config.address, impl_->config.port)) {
        impl_->port = impl_->config.port;
    } else {
        if (error != nullptr) {
            *error = "could not bind " + impl_->config.address + ":" +
                     std::to_string(impl_->config.port);
        }
        return false;
    }

    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    // Waiting for the accept loop rather than returning into a race: a caller
    // that connects the moment start() answers would otherwise sometimes arrive
    // before there is anything listening.
    impl_->server.wait_until_ready();
    return true;
}

void RemoteServer::stop() {
    if (impl_ == nullptr) {
        return;
    }
    // The gate first, so requests waiting on the interface thread are released
    // at once rather than each sitting out its full timeout before the listener
    // can join. Then the socket, then the thread -- and only after all three may
    // the owner destroy the IPlayerControl this holds a reference to.
    impl_->gate.close();
    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->port = 0;
}

int RemoteServer::boundPort() const { return impl_ == nullptr ? 0 : impl_->port; }

/// The docs page and its two assets, which are served without a token.
///
/// Not an exemption granted on principle -- a browser cannot put an
/// Authorization header on a top-level navigation, so a token-gated /docs is a
/// page nobody can open. The three files describe the page's own chrome and
/// nothing about this player; the specification behind them, and every endpoint,
/// still need the token.
bool isDocsAsset(std::string_view path) {
    return path == "/docs" || path == "/docs/" ||
           path == "/docs/swagger-ui.css" || path == "/docs/swagger-ui-bundle.js" ||
           path == "/docs/docs.js";
}

RawResponse RemoteServer::handle(const RawRequest& request) {
    // Before anything else, and with no exemption for where the connection came
    // from. A loopback exemption would mean every process on the machine holds
    // the transport, which is not the promise the preferences pane makes.
    if (!isDocsAsset(request.path) &&
        !constantTimeEquals(bearerToken(request.authorization), impl_->config.token)) {
        const std::chrono::milliseconds penalty = impl_->rateLimit.noteFailure(request.peer);
        if (penalty.count() > 0) {
            std::this_thread::sleep_for(penalty);
        }
        return unauthorized();
    }
    impl_->rateLimit.noteSuccess(request.peer);

    const Router::Match match = impl_->router.find(request.method, request.path);
    if (match.route == nullptr) {
        if (match.allowed.empty()) {
            return jsonError(404, "not_found", "No such endpoint.");
        }
        // The path exists under other methods, which is a different mistake from
        // a wrong path and worth saying so -- otherwise the reader goes looking
        // for a typo in a URL that is correct.
        RawResponse response = jsonError(
            405, "method_not_allowed", "That endpoint does not take this method.");
        std::string allow;
        for (const std::string_view method : match.allowed) {
            if (!allow.empty()) {
                allow += ", ";
            }
            allow += method;
        }
        response.headers.emplace_back("Allow", allow);
        return response;
    }

    if (match.route->writes && !impl_->config.allowWrite) {
        return jsonError(403, "read_only",
                         "This server is configured to allow reads only.");
    }

    // Parsed non-throwing, because this is bytes off a socket and an exception
    // escaping into httplib's worker is not a way to answer 400.
    nlohmann::json body = nlohmann::json::object();
    if (!request.body.empty()) {
        body = nlohmann::json::parse(request.body, nullptr, false);
        if (body.is_discarded()) {
            return jsonError(400, "bad_json", "The request body is not valid JSON.");
        }
        if (!body.is_object()) {
            return jsonError(400, "bad_json", "The request body must be a JSON object.");
        }
    }

    const Query query{request.query};
    const Ctx   ctx{impl_->gate, request, query, match.path, body};

    RawResponse response = match.route->handler(ctx);

    // On everything, and cheap. nosniff is what stops a JSON error body being
    // read as something executable by a browser that went looking.
    response.headers.emplace_back("X-Content-Type-Options", "nosniff");
    response.headers.emplace_back("Cache-Control", "no-store");
    // Deliberately no CORS headers at all. A page on another origin cannot reach
    // this even holding a stolen token, and there is nothing to preflight past.
    return response;
}


bool remoteServerAvailable() noexcept { return true; }

}  // namespace xpcog::remote
