// Routing, validation and the shapes of the answers.
//
// Everything here goes through RemoteServer::handle(), which is the whole
// request pipeline with no socket in it. That is what makes this suite possible
// at all, and it is why the one test that binds a port is testing the socket
// rather than the API.
//
// The dispatcher runs inline, so the gate resolves immediately and what is being
// tested is the router rather than the hop. The hop has its own suite.

#include "../FakePlayerControl.hpp"

#include "xpcog/core/remote/RemoteServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>

using namespace xpcog;
using namespace xpcog::remote;
using xpcog::test::FakePlayerControl;

#ifdef XPCOG_HAS_REST

namespace {

constexpr std::string_view kToken = "0123456789abcdef";

class Harness {
public:
    explicit Harness(bool allowWrite = true) {
        ServerConfig config;
        config.token      = std::string{kToken};
        config.allowWrite = allowWrite;
        server_ = std::make_unique<RemoteServer>(
            control_, [](std::function<void()> job) { job(); }, std::move(config));
    }

    [[nodiscard]] FakePlayerControl& control() { return control_; }

    RawResponse send(std::string method, std::string path, std::string body = {},
                     std::string query = {}) {
        RawRequest request;
        request.method        = std::move(method);
        request.path          = std::move(path);
        request.body          = std::move(body);
        request.query         = std::move(query);
        request.authorization = std::string{"Bearer "} + std::string{kToken};
        return server_->handle(request);
    }

    static nlohmann::json parse(const RawResponse& response) {
        nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
        REQUIRE_FALSE(body.is_discarded());
        return body;
    }

    static std::string header(const RawResponse& response, std::string_view name) {
        for (const auto& [key, value] : response.headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

private:
    FakePlayerControl             control_;
    std::unique_ptr<RemoteServer> server_;
};

}  // namespace

TEST_CASE("a path that exists under another method answers 405, not 404", "[remote]") {
    Harness harness;

    const RawResponse response = harness.send("DELETE", "/api/v1/status");

    REQUIRE(response.status == 405);
    // The distinction is the point: a wrong method and a wrong path are
    // different mistakes, and reporting both as 404 sends the reader looking for
    // a typo in a URL that is correct.
    CHECK(harness.header(response, "Allow") == "GET");
    CHECK(Harness::parse(response).at("error").at("code") == "method_not_allowed");
}

TEST_CASE("a path that exists under two methods lists both", "[remote]") {
    Harness           harness;
    const RawResponse response = harness.send("DELETE", "/api/v1/transport/volume");

    REQUIRE(response.status == 405);
    const std::string allow = harness.header(response, "Allow");
    CHECK(allow.find("GET") != std::string::npos);
    CHECK(allow.find("PUT") != std::string::npos);
}

TEST_CASE("a trailing slash is the same endpoint", "[remote]") {
    Harness harness;
    CHECK(harness.send("GET", "/api/v1/status/").status == 200);
}

TEST_CASE("status is served without asking the player twice", "[remote]") {
    Harness harness;
    harness.control().statusValue.playing      = true;
    harness.control().statusValue.position     = 12.5;
    harness.control().statusValue.currentTrack = 42;
    harness.control().statusValue.repeat       = "all";

    const RawResponse  response = harness.send("GET", "/api/v1/status");
    const nlohmann::json body   = Harness::parse(response);

    REQUIRE(response.status == 200);
    CHECK(body.at("playing").get<bool>());
    CHECK(body.at("position").get<double>() == 12.5);
    CHECK(body.at("currentTrack").get<TrackId>() == 42);
    CHECK(body.at("repeat").get<std::string>() == "all");
    CHECK(harness.control().countOf("status") == 1);
}

TEST_CASE("nothing playing reports a null track rather than zero", "[remote]") {
    Harness harness;
    // Zero is not a track id, and a client that treated it as one would be
    // asking about a track that cannot exist.
    harness.control().statusValue.currentTrack = kInvalidTrackId;

    const nlohmann::json body = Harness::parse(harness.send("GET", "/api/v1/status"));
    CHECK(body.at("currentTrack").is_null());
}

TEST_CASE("a transport command answers with the status afterwards", "[remote]") {
    Harness harness;
    harness.control().statusValue.playing = true;

    const RawResponse response = harness.send("POST", "/api/v1/transport/next");

    REQUIRE(response.status == 200);
    CHECK(harness.control().countOf("next") == 1);
    // Saves the client a second round trip for something this call already knows.
    CHECK(Harness::parse(response).at("playing").get<bool>());
}

TEST_CASE("a declined command is 409, not 200", "[remote]") {
    Harness harness;
    // PlaybackController silently declines while a start is in flight. Reporting
    // that as success would be the first place this API lied.
    harness.control().outcome = Outcome::Busy;

    const RawResponse response = harness.send("POST", "/api/v1/transport/playPause");

    REQUIRE(response.status == 409);
    CHECK(harness.header(response, "Retry-After") == "1");
    CHECK(Harness::parse(response).at("error").at("code") == "busy");
}

TEST_CASE("play takes an optional track id", "[remote]") {
    Harness harness;

    CHECK(harness.send("POST", "/api/v1/transport/play").status == 200);
    CHECK(harness.send("POST", "/api/v1/transport/play", R"({"trackId":7})").status == 200);

    const auto calls = harness.control().calls();
    std::size_t withId = 0;
    for (const auto& call : calls) {
        if (call.name == "play" && !call.ids.empty()) {
            ++withId;
            CHECK(call.ids.front() == 7);
        }
    }
    CHECK(withId == 1);
}

TEST_CASE("a bad field is 400 and says which field", "[remote]") {
    Harness harness;

    const RawResponse response =
        harness.send("POST", "/api/v1/transport/play", R"({"trackId":"seven"})");

    REQUIRE(response.status == 400);
    // Naming the field is most of the value: "bad request" with no field is a
    // client author reading the whole payload again.
    CHECK(Harness::parse(response).at("error").at("field") == "trackId");
}

TEST_CASE("a body that is not JSON is 400 rather than an exception", "[remote]") {
    Harness harness;

    const RawResponse response =
        harness.send("PUT", "/api/v1/transport/volume", "{not json");
    REQUIRE(response.status == 400);
    CHECK(Harness::parse(response).at("error").at("code") == "bad_json");

    // An array parses but is not what any route takes.
    const RawResponse array = harness.send("PUT", "/api/v1/transport/volume", "[1,2]");
    CHECK(array.status == 400);
}

TEST_CASE("volume is bounded at the edge rather than clamped quietly", "[remote]") {
    Harness harness;

    CHECK(harness.send("PUT", "/api/v1/transport/volume", R"({"volume":0.5})").status == 200);
    // Clamping would answer 200 having done something else, and the response
    // would agree with neither the request nor the speakers.
    CHECK(harness.send("PUT", "/api/v1/transport/volume", R"({"volume":1.5})").status == 400);
    CHECK(harness.send("PUT", "/api/v1/transport/volume", R"({"volume":-1})").status == 400);
    CHECK(harness.send("PUT", "/api/v1/transport/volume", R"({"volume":true})").status == 400);
    CHECK(harness.send("PUT", "/api/v1/transport/volume", "{}").status == 400);

    CHECK(harness.control().countOf("setVolume") == 1);
}

TEST_CASE("seek takes seconds or a fraction, and not both", "[remote]") {
    Harness harness;
    harness.control().statusValue.duration = 200.0;

    CHECK(harness.send("POST", "/api/v1/transport/seek", R"({"seconds":30})").status == 200);
    CHECK(harness.send("POST", "/api/v1/transport/seek", R"({"fraction":0.5})").status == 200);
    CHECK(harness.send("POST", "/api/v1/transport/seek", "{}").status == 400);
    CHECK(harness.send("POST", "/api/v1/transport/seek",
                       R"({"seconds":1,"fraction":0.5})").status == 400);
    CHECK(harness.send("POST", "/api/v1/transport/seek", R"({"fraction":2})").status == 400);

    // The fraction is resolved here, against the duration this end already
    // knows, rather than making the client fetch it first.
    bool sawHalf = false;
    for (const auto& call : harness.control().calls()) {
        if (call.name == "seek" && call.number == 100.0) {
            sawHalf = true;
        }
    }
    CHECK(sawHalf);
}

TEST_CASE("seeking by fraction with no duration is refused", "[remote]") {
    Harness harness;
    // A stream has no duration, so there is no position a fraction names.
    harness.control().statusValue.duration = 0.0;

    const RawResponse response =
        harness.send("POST", "/api/v1/transport/seek", R"({"fraction":0.5})");
    CHECK(response.status == 409);
}

TEST_CASE("order takes any one of its three", "[remote]") {
    Harness harness;

    CHECK(harness.send("PUT", "/api/v1/transport/order", R"({"repeat":"all"})").status == 200);
    CHECK(harness.send("PUT", "/api/v1/transport/order",
                       R"({"stopAfterCurrent":true})").status == 200);
    // Nothing at all is a request that means nothing, and a 200 for it would be
    // indistinguishable from one that worked.
    CHECK(harness.send("PUT", "/api/v1/transport/order", "{}").status == 400);
}

TEST_CASE("a read-only server refuses writes and still serves reads", "[remote]") {
    Harness harness(/*allowWrite=*/false);

    CHECK(harness.send("GET", "/api/v1/status").status == 200);
    CHECK(harness.send("GET", "/api/v1/transport/volume").status == 200);

    const RawResponse response = harness.send("POST", "/api/v1/transport/stop");
    REQUIRE(response.status == 403);
    CHECK(Harness::parse(response).at("error").at("code") == "read_only");
    // Refused before it reached the player, not after.
    CHECK(harness.control().countOf("stop") == 0);
}

TEST_CASE("every response carries the sniffing and caching headers", "[remote]") {
    Harness           harness;
    const RawResponse response = harness.send("GET", "/api/v1/status");

    CHECK(harness.header(response, "X-Content-Type-Options") == "nosniff");
    CHECK(harness.header(response, "Cache-Control") == "no-store");

    // Deliberately absent: a page on another origin cannot reach this even
    // holding a stolen token, and there is nothing to preflight past.
    CHECK(harness.header(response, "Access-Control-Allow-Origin").empty());
}

#endif  // XPCOG_HAS_REST
