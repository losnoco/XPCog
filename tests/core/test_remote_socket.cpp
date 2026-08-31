// The one test that opens a socket.
//
// Everything else drives handle() directly, which is the whole request pipeline
// with no network in it. What that cannot reach is the part this covers: bind,
// accept, the header conversion in and out, the thread pool, and stopping while
// a request is in flight.
//
// It binds 127.0.0.1 on port 0, so it never collides with whatever else is
// listening on the machine running it, and it runs by default. A hidden tag was
// considered and rejected: a socket test that skips in every sandbox is
// coverage quietly going away, which is what CLAUDE.md means about watching the
// skips. XPCOG_NO_SOCKET_TESTS=1 skips it loudly for an environment that really
// forbids listening.

#include "../FakePlayerControl.hpp"

#include "xpcog/core/remote/RemoteServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace xpcog;
using namespace xpcog::remote;
using xpcog::test::FakePlayerControl;

namespace {

constexpr const char* kToken = "0123456789abcdef";

bool socketsForbidden() {
    const char* value = std::getenv("XPCOG_NO_SOCKET_TESTS");
    return value != nullptr && *value != '\0' && std::string{value} != "0";
}

}  // namespace

TEST_CASE("a bound server answers over a real socket", "[remote][socket]") {
    if (socketsForbidden()) {
        SKIP("XPCOG_NO_SOCKET_TESTS is set");
    }

    FakePlayerControl control;
    control.statusValue.playing  = true;
    control.statusValue.position = 42.0;

    ServerConfig config;
    config.token   = kToken;
    config.address = "127.0.0.1";
    config.port    = 0;

    RemoteServer server{control, [](std::function<void()> job) { job(); },
                        std::move(config)};

    std::string error;
    REQUIRE(server.start(&error));
    // Port 0 asked for any free one, and the server has to be able to say which
    // it got or nothing could connect to it.
    const int port = server.boundPort();
    REQUIRE(port > 0);

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(5, 0);

    SECTION("an authenticated request is answered") {
        httplib::Headers headers{{"Authorization", std::string{"Bearer "} + kToken}};
        auto             response = client.Get("/api/v1/status", headers);

        REQUIRE(response);
        CHECK(response->status == 200);

        const nlohmann::json body =
            nlohmann::json::parse(response->body, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        CHECK(body.at("playing").get<bool>());
        CHECK(body.at("position").get<double>() == 42.0);

        // The headers handle() sets have to survive the conversion out.
        CHECK(response->get_header_value("X-Content-Type-Options") == "nosniff");
    }

    SECTION("an unauthenticated request is refused over the wire too") {
        auto response = client.Get("/api/v1/status");

        REQUIRE(response);
        CHECK(response->status == 401);
        CHECK(response->get_header_value("WWW-Authenticate").find("Bearer") !=
              std::string::npos);
    }

    SECTION("a POST carries its body through") {
        httplib::Headers headers{{"Authorization", std::string{"Bearer "} + kToken}};
        auto response = client.Post("/api/v1/transport/seek", headers,
                                    R"({"seconds":12})", "application/json");

        REQUIRE(response);
        CHECK(response->status == 200);
        CHECK(control.countOf("seek") == 1);
    }

    SECTION("the docs page is served without a token, and is usable") {
        // The one deliberate hole, and it is forced: a browser cannot put an
        // Authorization header on a top-level navigation, so a token-gated
        // /docs is a page nobody can open.
        auto page = client.Get("/docs");
        REQUIRE(page);
        CHECK(page->status == 200);
        CHECK(page->get_header_value("Content-Type").find("text/html") !=
              std::string::npos);
        CHECK(page->body.find("swagger-ui-bundle.js") != std::string::npos);
        // It has to be allowed to load its own two files and reach this server,
        // and nothing else.
        CHECK(page->get_header_value("Content-Security-Policy").find("default-src 'none'") !=
              std::string::npos);

        // The real script arrives, whichever side of the encoding negotiation
        // this client happens to take -- httplib expands a gzip body itself when
        // it can. Which branch the server picks, and that it never sends an
        // encoding nobody asked for, is asserted deterministically in
        // test_remote_router.cpp; here the question is only whether a client can
        // fetch a working page over a socket.
        auto script = client.Get("/docs/swagger-ui-bundle.js");
        INFO("client error: " << httplib::to_string(script.error()));
        REQUIRE(script);
        CHECK(script->status == 200);
        CHECK(script->body.size() > 1000000);

        auto css = client.Get("/docs/swagger-ui.css");
        REQUIRE(css);
        CHECK(css->status == 200);
        CHECK(css->body.find(".swagger-ui") != std::string::npos);
    }

    SECTION("the specification itself still needs a token") {
        // The page is open; what it describes is not. Otherwise /docs would be a
        // way to read the whole API surface without one.
        auto refused = client.Get("/openapi.json");
        REQUIRE(refused);
        CHECK(refused->status == 401);

        httplib::Headers headers{{"Authorization", std::string{"Bearer "} + kToken}};
        auto             allowed = client.Get("/openapi.json", headers);
        REQUIRE(allowed);
        CHECK(allowed->status == 200);
        CHECK(allowed->body.find("\"openapi\"") != std::string::npos);
    }

    SECTION("a query string arrives") {
        httplib::Headers headers{{"Authorization", std::string{"Bearer "} + kToken}};
        auto response = client.Get("/api/v1/playlist?limit=5&q=blue", headers);

        REQUIRE(response);
        CHECK(response->status == 200);

        bool sawFilter = false;
        for (const auto& call : control.calls()) {
            if (call.name == "tracks" && call.text == "blue") {
                sawFilter = true;
            }
        }
        CHECK(sawFilter);
    }

    server.stop();
}

TEST_CASE("stopping releases the port", "[remote][socket]") {
    if (socketsForbidden()) {
        SKIP("XPCOG_NO_SOCKET_TESTS is set");
    }

    FakePlayerControl control;

    ServerConfig config;
    config.token = kToken;
    config.port  = 0;

    auto server = std::make_unique<RemoteServer>(
        control, [](std::function<void()> job) { job(); }, config);
    REQUIRE(server->start());
    const int port = server->boundPort();
    REQUIRE(port > 0);

    server->stop();
    CHECK(server->boundPort() == 0);

    // Calling it twice is what the destructor does after an explicit stop, and
    // it must not be a second attempt at joining a thread that has gone.
    server->stop();
    server.reset();

    // The port is free again: a fresh server can take it. This is the property
    // that matters when the preferences pane restarts the server after a port
    // change.
    ServerConfig again;
    again.token = kToken;
    again.port  = port;
    RemoteServer successor{control, [](std::function<void()> job) { job(); },
                           std::move(again)};
    std::string  error;
    CHECK(successor.start(&error));
}

TEST_CASE("a server that cannot bind says so rather than pretending",
          "[remote][socket]") {
    if (socketsForbidden()) {
        SKIP("XPCOG_NO_SOCKET_TESTS is set");
    }

    FakePlayerControl control;

    ServerConfig first;
    first.token = kToken;
    first.port  = 0;
    RemoteServer holder{control, [](std::function<void()> job) { job(); },
                        std::move(first)};
    REQUIRE(holder.start());

    // The ordinary failure: the port is already taken. The preferences pane
    // shows what this says, so it has to say something.
    ServerConfig second;
    second.token = kToken;
    second.port  = holder.boundPort();
    RemoteServer clash{control, [](std::function<void()> job) { job(); },
                       std::move(second)};

    std::string error;
    CHECK_FALSE(clash.start(&error));
    CHECK_FALSE(error.empty());
    CHECK(clash.boundPort() == 0);
}
