// The remote control's skeleton: the request pipeline, with no socket.
//
// handle() is public precisely so this is possible. Everything a request goes
// through -- routing, and later auth, validation and dispatch -- happens behind
// it and answers with a struct, so the whole API can be exercised here without
// binding a port, and the one test that does bind is testing the socket rather
// than the API. That is IHttpClient's arrangement pointed the other way.

#include "xpcog/core/Version.hpp"
#include "xpcog/core/remote/PlayerControl.hpp"
#include "xpcog/core/remote/RemoteServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

using namespace xpcog;
using namespace xpcog::remote;

namespace {

constexpr std::string_view kToken = "0123456789abcdef";

/// Answers nothing, because nothing is asked of it yet. The routes that reach
/// the player bring the methods and the fake that scripts them.
struct StubControl : IPlayerControl {};

RemoteServer makeServer(StubControl& control) {
    ServerConfig config;
    config.token = std::string{kToken};
    // Runs inline. Nothing here hops yet, and when it does, an inline dispatcher
    // is the case that would deadlock a gate that took its lock first.
    return RemoteServer{control, [](std::function<void()> job) { job(); },
                        std::move(config)};
}

RawRequest get(std::string path) {
    RawRequest request;
    request.method        = "GET";
    request.path          = std::move(path);
    request.authorization = std::string{"Bearer "} + std::string{kToken};
    return request;
}

}  // namespace

TEST_CASE("the build answers about whether it has a server", "[remote]") {
#ifdef XPCOG_HAS_REST
    CHECK(remoteServerAvailable());
#else
    CHECK_FALSE(remoteServerAvailable());
#endif
}

#ifdef XPCOG_HAS_REST

TEST_CASE("version is served as JSON", "[remote]") {
    StubControl       control;
    RemoteServer      server   = makeServer(control);
    const RawResponse response = server.handle(get("/api/v1/version"));

    REQUIRE(response.status == 200);
    CHECK(response.contentType == "application/json; charset=utf-8");

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK(body.at("version").get<std::string>() == std::string{kVersionString});
    CHECK(body.at("apiVersion").get<int>() == 1);
}

TEST_CASE("an unknown path is a JSON 404, not an empty body", "[remote]") {
    StubControl       control;
    RemoteServer      server   = makeServer(control);
    const RawResponse response = server.handle(get("/api/v1/nothing-here"));

    REQUIRE(response.status == 404);

    // The shape every error takes, and it is worth pinning from the first one:
    // a client that has to distinguish "wrong endpoint" from "wrong token" reads
    // the code, not the prose.
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK(body.at("error").at("code").get<std::string>() == "not_found");
    CHECK_FALSE(body.at("error").at("message").get<std::string>().empty());
}

TEST_CASE("a server with no token refuses to start", "[remote]") {
    // Not a convenience worth having. An empty token would mean every process
    // that can reach the port owns the transport.
    StubControl  control;
    ServerConfig config;
    config.token = "";
    RemoteServer server{control, [](std::function<void()> job) { job(); },
                        std::move(config)};

    std::string error;
    CHECK_FALSE(server.start(&error));
    CHECK_FALSE(error.empty());
    CHECK(server.boundPort() == 0);
}

TEST_CASE("the OpenAPI document is 3.1 and carries this version", "[remote]") {
    const nlohmann::json document =
        nlohmann::json::parse(RemoteServer::openApiDocument(), nullptr, false);
    REQUIRE_FALSE(document.is_discarded());
    CHECK(document.at("openapi").get<std::string>() == "3.1.0");
    CHECK(document.at("info").at("version").get<std::string>() ==
          std::string{kVersionString});
}

TEST_CASE("every way of failing to authenticate looks the same", "[remote]") {
    StubControl  control;
    RemoteServer server = makeServer(control);

    RawRequest missing;
    missing.method = "GET";
    missing.path   = "/api/v1/version";

    RawRequest malformed = missing;
    malformed.authorization = "Basic aGVsbG86d29ybGQ=";

    RawRequest wrong = missing;
    wrong.authorization = "Bearer fedcba9876543210";

    // Same length as the real one, differing in the last character: the case a
    // timing side channel would be about.
    RawRequest nearly = missing;
    nearly.authorization = "Bearer 0123456789abcdee";

    const RawResponse a = server.handle(missing);
    const RawResponse b = server.handle(malformed);
    const RawResponse c = server.handle(wrong);
    const RawResponse d = server.handle(nearly);

    for (const RawResponse* response : {&a, &b, &c, &d}) {
        CHECK(response->status == 401);
        // Byte-identical, deliberately. A client that could tell these apart
        // could learn something about the token it should not.
        CHECK(response->body == a.body);
    }

    const nlohmann::json body = nlohmann::json::parse(a.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    CHECK(body.at("error").at("code").get<std::string>() == "unauthorized");

    // A browser needs to be told what to ask for.
    bool announced = false;
    for (const auto& [name, value] : a.headers) {
        if (name == "WWW-Authenticate") {
            announced = true;
            CHECK(value.find("Bearer") != std::string::npos);
        }
    }
    CHECK(announced);
}

TEST_CASE("the scheme is matched case-insensitively", "[remote]") {
    StubControl  control;
    RemoteServer server = makeServer(control);

    // RFC 7235 says the scheme is case-insensitive, so a client that sends
    // "bearer" is not wrong and should not be told it is.
    RawRequest request;
    request.method        = "GET";
    request.path          = "/api/v1/version";
    request.authorization = "bearer 0123456789abcdef";

    CHECK(server.handle(request).status == 200);
}

#endif  // XPCOG_HAS_REST
