// The ListenBrainz protocol, pinned against a scripted transport.
//
// The interesting properties are the ones the server would only tell you
// about by refusing: the header the token travels in, the fields a listen
// must and must not carry, and which status codes the queue should wait out
// versus give up on. The last of those is what a queue's correctness rests on,
// and the server's answer to getting it wrong is a backlog that never drains
// or one that vanishes.

#include "../FakeHttp.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/core/scrobble/ListenBrainzClient.hpp"
#include "xpcog/core/scrobble/Scrobbler.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace xpcog;
using xpcog::test::FakeHttp;
using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kToken = "0123abcd-user-token";
constexpr std::string_view kOk    = R"({"status":"ok"})";

[[nodiscard]] ScrobbleTrack track(std::int64_t startedAt) {
    ScrobbleTrack t;
    t.artist      = "Boards of Canada";
    t.title       = "Roygbiv";
    t.album       = "Music Has the Right to Children";
    t.trackNumber = 6;
    t.duration    = 152.5;
    t.startedAt   = startedAt;
    return t;
}

[[nodiscard]] json sentBody(const FakeHttp& http, std::size_t index = 0) {
    const auto calls = http.calls();
    REQUIRE(index < calls.size());
    return json::parse(calls[index].body);
}

}  // namespace

// --- requests ------------------------------------------------------------

TEST_CASE("The token travels in the Authorization header", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, std::string{kOk});

    CHECK(client.updateNowPlaying(track(0), kToken));
    REQUIRE(http.callCount() == 1);
    CHECK(http.header(0, "Authorization") == "Token " + std::string{kToken});
    CHECK(http.calls()[0].url == "https://api.listenbrainz.org/1/submit-listens");
    CHECK(http.calls()[0].post);
}

TEST_CASE("A now-playing announcement carries no timestamp", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, std::string{kOk});

    CHECK(client.updateNowPlaying(track(1'700'000'000), kToken));

    const json body = sentBody(http);
    CHECK(body["listen_type"] == "playing_now");
    REQUIRE(body["payload"].size() == 1);
    const json& listen = body["payload"][0];
    // The server refuses a playing_now with one, so its absence is the point.
    CHECK_FALSE(listen.contains("listened_at"));
    CHECK(listen["track_metadata"]["artist_name"] == "Boards of Canada");
    CHECK(listen["track_metadata"]["track_name"] == "Roygbiv");
    CHECK(listen["track_metadata"]["release_name"] == "Music Has the Right to Children");
}

TEST_CASE("One play is a single and several are an import", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.setDefaultReply(200, std::string{kOk});

    const std::vector<ScrobbleTrack> one = {track(1'700'000'000)};
    auto result = client.scrobble(one, kToken);
    REQUIRE(result);
    CHECK(result->accepted == 1);
    CHECK(sentBody(http, 0)["listen_type"] == "single");
    CHECK(sentBody(http, 0)["payload"][0]["listened_at"] == 1'700'000'000);

    const std::vector<ScrobbleTrack> three = {track(1'700'000'000), track(1'700'000'200),
                                              track(1'700'000'400)};
    result = client.scrobble(three, kToken);
    REQUIRE(result);
    CHECK(result->accepted == 3);
    const json body = sentBody(http, 1);
    CHECK(body["listen_type"] == "import");
    REQUIRE(body["payload"].size() == 3);
    CHECK(body["payload"][2]["listened_at"] == 1'700'000'400);
}

TEST_CASE("A listen says which program sent it, and what it knows about the track",
          "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, std::string{kOk});

    const std::vector<ScrobbleTrack> one = {track(1'700'000'000)};
    REQUIRE(client.scrobble(one, kToken));

    const json info = sentBody(http)["payload"][0]["track_metadata"]["additional_info"];
    CHECK(info["submission_client"] == "XPCog");
    CHECK(info["submission_client_version"] == std::string{kVersionString});
    CHECK(info["media_player"] == "XPCog");
    // A string, as the API documents it, not a number.
    CHECK(info["tracknumber"] == "6");
    CHECK(info["duration_ms"] == 152500);
}

TEST_CASE("Empty fields are left out rather than sent empty", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, std::string{kOk});

    ScrobbleTrack bare;
    bare.artist    = "Someone";
    bare.title     = "Something";
    bare.startedAt = 1'700'000'000;
    const std::vector<ScrobbleTrack> one = {bare};
    REQUIRE(client.scrobble(one, kToken));

    const json metadata = sentBody(http)["payload"][0]["track_metadata"];
    CHECK_FALSE(metadata.contains("release_name"));
    CHECK_FALSE(metadata["additional_info"].contains("tracknumber"));
    CHECK_FALSE(metadata["additional_info"].contains("duration_ms"));
    CHECK_FALSE(metadata["additional_info"].contains("recording_mbid"));
}

TEST_CASE("Batches beyond the limit are refused before being sent", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};

    const std::vector<ScrobbleTrack> tooMany(ListenBrainzClient::kMaxBatch + 1,
                                             track(1'700'000'000));
    ScrobbleError error;
    CHECK_FALSE(client.scrobble(tooMany, kToken, &error));
    CHECK(error.kind == ScrobbleError::Kind::Api);
    CHECK(http.callCount() == 0);

    const std::vector<ScrobbleTrack> none;
    CHECK_FALSE(client.scrobble(none, kToken, &error));
    CHECK(http.callCount() == 0);
}

TEST_CASE("Nothing is sent without a token", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};

    ScrobbleError error;
    CHECK_FALSE(client.updateNowPlaying(track(0), "", &error));
    CHECK(error.kind == ScrobbleError::Kind::SessionInvalid);
    CHECK(http.callCount() == 0);
}

// --- the server root ------------------------------------------------------

TEST_CASE("The API root is taken in either spelling", "[listenbrainz]") {
    FakeHttp http;
    http.setDefaultReply(200, std::string{kOk});

    const auto urlFor = [&http](const std::string& root) {
        ListenBrainzClient client{http, root};
        REQUIRE(client.updateNowPlaying(track(0), kToken));
        return http.calls().back().url;
    };

    CHECK(urlFor("https://api.listenbrainz.org") ==
          "https://api.listenbrainz.org/1/submit-listens");
    CHECK(urlFor("https://api.listenbrainz.org/") ==
          "https://api.listenbrainz.org/1/submit-listens");
    CHECK(urlFor("https://api.listenbrainz.org/1") ==
          "https://api.listenbrainz.org/1/submit-listens");
    // Maloja's compatibility endpoint, which is where a self-hoster points.
    CHECK(urlFor("http://maloja.local:42010/apis/listenbrainz") ==
          "http://maloja.local:42010/apis/listenbrainz/1/submit-listens");
    // Empty falls back to the public service rather than to a request at "/1".
    CHECK(urlFor("") == "https://api.listenbrainz.org/1/submit-listens");
}

TEST_CASE("The root can be changed after construction", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.setDefaultReply(200, std::string{kOk});

    client.setApiRoot("https://lb.example.org");
    CHECK(client.apiRoot() == "https://lb.example.org");
    REQUIRE(client.updateNowPlaying(track(0), kToken));
    CHECK(http.calls().back().url == "https://lb.example.org/1/submit-listens");
}

// --- validating a token ---------------------------------------------------

TEST_CASE("A valid token answers with its owner", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, R"({"code":200,"message":"Token valid.","valid":true,"user_name":"kevin"})");

    ScrobbleError error;
    const auto    user = client.validateToken(kToken, &error);
    REQUIRE(user);
    CHECK(*user == "kevin");
    CHECK(error.ok());
    CHECK(http.calls()[0].url == "https://api.listenbrainz.org/1/validate-token");
    CHECK_FALSE(http.calls()[0].post);
    CHECK(http.header(0, "Authorization") == "Token " + std::string{kToken});
}

TEST_CASE("An unknown token is reported as invalid, not as a failure to ask",
          "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    // 200, not 401: the server answers the question rather than refusing it.
    http.reply(200, R"({"code":200,"message":"Token invalid.","valid":false})");

    ScrobbleError error;
    CHECK_FALSE(client.validateToken(kToken, &error));
    CHECK(error.kind == ScrobbleError::Kind::SessionInvalid);
    CHECK(error.message == "Token invalid.");
}

TEST_CASE("A reply that is not the server's is malformed", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.reply(200, "<html>captive portal</html>");

    ScrobbleError error;
    CHECK_FALSE(client.validateToken(kToken, &error));
    CHECK(error.kind == ScrobbleError::Kind::Malformed);
    CHECK(error.retryable());
}

// --- verdicts -------------------------------------------------------------

TEST_CASE("401 invalidates the session; 429 and 5xx wait; 400 gives up",
          "[listenbrainz]") {
    const auto errorFor = [](long status, std::string body) {
        FakeHttp           http;
        ListenBrainzClient client{http};
        http.reply(status, std::move(body));
        ScrobbleError                    error;
        const std::vector<ScrobbleTrack> one = {track(1'700'000'000)};
        CHECK_FALSE(client.scrobble(one, kToken, &error));
        return error;
    };

    const auto unauthorized = errorFor(401, R"({"code":401,"error":"Invalid authorization token."})");
    CHECK(unauthorized.kind == ScrobbleError::Kind::SessionInvalid);
    CHECK_FALSE(unauthorized.retryable());
    CHECK(unauthorized.message == "Invalid authorization token.");

    CHECK(errorFor(429, "").kind == ScrobbleError::Kind::Transient);
    CHECK(errorFor(429, "").retryable());
    CHECK(errorFor(503, "").retryable());
    CHECK(errorFor(502, "<html>bad gateway</html>").retryable());

    const auto bad = errorFor(400, R"({"code":400,"error":"JSON document must contain a payload"})");
    CHECK(bad.kind == ScrobbleError::Kind::Api);
    CHECK_FALSE(bad.retryable());
    CHECK(bad.code == 400);
    CHECK(bad.message == "JSON document must contain a payload");
}

TEST_CASE("A transport failure is retryable", "[listenbrainz]") {
    FakeHttp           http;
    ListenBrainzClient client{http};
    http.failTransport("could not resolve host");

    ScrobbleError error;
    CHECK_FALSE(client.updateNowPlaying(track(0), kToken, &error));
    CHECK(error.kind == ScrobbleError::Kind::Transport);
    CHECK(error.retryable());
    CHECK(error.message == "could not resolve host");
}

// --- through the queue ----------------------------------------------------

TEST_CASE("The queue drains through ListenBrainz as it does through Last.fm",
          "[listenbrainz][scrobbler]") {
    const fs::path dir = fs::temp_directory_path() / "xpcog-scrobble-listenbrainz";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path queuePath = dir / "queue.json";

    FakeHttp           http;
    ListenBrainzClient client{http};
    constexpr std::int64_t kNow = 1'700'000'000;

    {
        Scrobbler scrobbler{client, queuePath, [] { return kNow; }};
        scrobbler.setSession(Scrobbler::Session{std::string{kToken}, "kevin"});
        scrobbler.setEnabled(true);
        REQUIRE(scrobbler.active());

        // The first attempt meets the rate limit; the play must survive it.
        http.reply(429, "");
        scrobbler.submit(track(kNow - 300));
        REQUIRE(scrobbler.drain(5s));
        CHECK(scrobbler.pending() == 1);

        // Then the server takes it.
        http.setDefaultReply(200, std::string{kOk});
        scrobbler.wake();
        REQUIRE(scrobbler.drain(5s));
        CHECK(scrobbler.pending() == 0);
    }

    // Two submissions of the same play, both to the listens endpoint, both
    // carrying the token.
    REQUIRE(http.callCount() == 2);
    for (const auto& call : http.calls()) {
        CHECK(call.url == "https://api.listenbrainz.org/1/submit-listens");
        CHECK(json::parse(call.body)["listen_type"] == "single");
    }
    CHECK(http.header(1, "Authorization") == "Token " + std::string{kToken});

    fs::remove_all(dir, ec);
}

TEST_CASE("A token the server withdraws clears the session", "[listenbrainz][scrobbler]") {
    const fs::path dir = fs::temp_directory_path() / "xpcog-scrobble-listenbrainz-401";
    std::error_code ec;
    fs::remove_all(dir, ec);

    FakeHttp           http;
    ListenBrainzClient client{http};
    http.setDefaultReply(401, R"({"code":401,"error":"Invalid authorization token."})");

    Scrobbler scrobbler{client, dir / "queue.json", [] { return 1'700'000'000; }};
    scrobbler.setSession(Scrobbler::Session{std::string{kToken}, "kevin"});
    scrobbler.setEnabled(true);

    std::atomic<bool> invalidated{false};
    scrobbler.onSessionInvalidated([&invalidated] { invalidated = true; });

    scrobbler.submit(track(1'700'000'000 - 300));
    REQUIRE(scrobbler.drain(5s));

    CHECK(invalidated);
    CHECK_FALSE(scrobbler.session().connected());
    // Kept: the play happened, and a reconnected listener should still get it.
    CHECK(scrobbler.pending() == 1);

    fs::remove_all(dir, ec);
}
