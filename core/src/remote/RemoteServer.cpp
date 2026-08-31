#include "xpcog/core/remote/RemoteServer.hpp"

#include "xpcog/core/Version.hpp"

// Included here and nowhere a public header can see it. httplib.h pulls in
// <winsock2.h> on Windows, and a core header dragging that into every
// translation unit that includes it would be a fresh category of problem -- so
// the server lives behind Impl and the include stays in this file.
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace xpcog::remote {
namespace {

RawResponse jsonError(int status, std::string_view code, std::string_view message) {
    RawResponse response;
    response.status = status;
    nlohmann::json body;
    body["error"]["code"]    = code;
    body["error"]["message"] = message;
    response.body            = body.dump();
    return response;
}

}  // namespace

struct RemoteServer::Impl {
    IPlayerControl& control;
    Dispatcher      dispatch;
    ServerConfig    config;

    httplib::Server server;
    std::thread     thread;
    int             port = 0;

    Impl(IPlayerControl& c, Dispatcher d, ServerConfig cfg)
        : control(c), dispatch(std::move(d)), config(std::move(cfg)) {}
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

    // One handler for everything: routing is handle()'s job, and cpp-httplib's
    // own router would be a second table to keep in step with the one the
    // OpenAPI document is generated from.
    impl_->server.set_pre_routing_handler(
        [this](const httplib::Request& in, httplib::Response& out) {
            RawRequest request;
            request.method        = in.method;
            request.path          = in.path;
            request.query         = in.target.find('?') == std::string::npos
                                        ? std::string{}
                                        : in.target.substr(in.target.find('?') + 1);
            request.body          = in.body;
            request.contentType   = in.get_header_value("Content-Type");
            request.authorization = in.get_header_value("Authorization");
            request.peer          = in.remote_addr;

            const RawResponse answer = handle(request);
            for (const auto& [name, value] : answer.headers) {
                out.set_header(name, value);
            }
            out.status = answer.status;
            out.set_content(answer.body, answer.contentType);
            return httplib::Server::HandlerResponse::Handled;
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

    impl_->port = impl_->server.bind_to_port(impl_->config.address, impl_->config.port);
    if (impl_->port <= 0) {
        if (error != nullptr) {
            *error = "could not bind " + impl_->config.address + ":" +
                     std::to_string(impl_->config.port);
        }
        return false;
    }

    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    return true;
}

void RemoteServer::stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->port = 0;
}

int RemoteServer::boundPort() const { return impl_ == nullptr ? 0 : impl_->port; }

RawResponse RemoteServer::handle(const RawRequest& request) {
    if (request.method == "GET" && request.path == "/api/v1/version") {
        nlohmann::json body;
        body["version"]    = kVersionString;
        body["apiVersion"] = 1;
        RawResponse response;
        response.body = body.dump();
        return response;
    }

    return jsonError(404, "not_found", "No such endpoint.");
}

std::string RemoteServer::openApiDocument() {
    nlohmann::json document;
    document["openapi"]      = "3.1.0";
    document["info"]["title"]   = "XPCog";
    document["info"]["version"] = kVersionString;
    document["paths"]           = nlohmann::json::object();
    return document.dump(2);
}

bool remoteServerAvailable() noexcept { return true; }

}  // namespace xpcog::remote
