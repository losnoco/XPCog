// The remote control, in a build configured without XPCOG_WITH_REST.
//
// Exactly one of this and RemoteServer.cpp is compiled, so every caller links
// the same symbols and none of them carries an #ifdef -- NullHttpClient.cpp's
// arrangement, for NullHttpClient.cpp's reason. remoteServerAvailable() answers
// false and the preferences pane greys itself and says why, which is the same
// shape as the crash-reporting checkbox in a build without Sentry.
//
// Stubs rather than throws: a player built without a server is not a broken
// player, it is a player that cannot be driven over HTTP. start() failing with a
// reason is something the pane can show; an exception is not.

#include "xpcog/core/remote/RemoteServer.hpp"

#include <utility>

namespace xpcog::remote {

struct RemoteServer::Impl {};

RemoteServer::RemoteServer(IPlayerControl& /*control*/, Dispatcher /*dispatch*/,
                           ServerConfig /*config*/) {}

RemoteServer::~RemoteServer() = default;

bool RemoteServer::start(std::string* error) {
    if (error != nullptr) {
        *error = "this build has no remote-control server";
    }
    return false;
}

void RemoteServer::stop() {}

int RemoteServer::boundPort() const { return 0; }

RawResponse RemoteServer::handle(const RawRequest& /*request*/) {
    RawResponse response;
    response.status = 501;
    response.body   = R"({"error":{"code":"not_built",)"
                    R"("message":"This build has no remote-control server."}})";
    return response;
}

std::string RemoteServer::openApiDocument() { return "{}"; }

bool remoteServerAvailable() noexcept { return false; }

}  // namespace xpcog::remote
