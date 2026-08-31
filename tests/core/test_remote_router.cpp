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
#include <memory>
#include <vector>
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

    /// A GET saying what it can decode, for the docs assets.
    RawResponse sendAccepting(std::string path, std::string encoding) {
        RawRequest request;
        request.method         = "GET";
        request.path           = std::move(path);
        request.acceptEncoding = std::move(encoding);
        return server_->handle(request);
    }

    RawResponse handleRaw(const RawRequest& request) { return server_->handle(request); }

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

// --- playlist ---------------------------------------------------------------

namespace {

TrackSummary summary(TrackId id, std::string title) {
    TrackSummary track;
    track.id    = id;
    track.title = std::move(title);
    return track;
}

}  // namespace

TEST_CASE("now playing answers the whole question in one request", "[remote]") {
    Harness harness;
    harness.control().statusValue.playing      = true;
    harness.control().statusValue.currentTrack = 42;
    harness.control().statusValue.position     = 91.3;
    harness.control().statusValue.duration     = 247.0;

    TrackDetail detail;
    detail.summary = summary(42, "So What");
    detail.summary.artist = "Miles Davis";
    detail.genre   = "Jazz";
    harness.control().trackDetail = detail;

    const nlohmann::json body =
        Harness::parse(harness.send("GET", "/api/v1/nowplaying"));

    CHECK(body.at("playing").get<bool>());
    CHECK(body.at("position").get<double>() == 91.3);
    CHECK(body.at("duration").get<double>() == 247.0);
    // The tags, not just an id to go and look up.
    CHECK(body.at("track").at("title").get<std::string>() == "So What");
    CHECK(body.at("track").at("artist").get<std::string>() == "Miles Davis");
    CHECK(body.at("track").at("genre").get<std::string>() == "Jazz");
}

TEST_CASE("now playing is one hop, not two", "[remote]") {
    Harness harness;
    harness.control().statusValue.currentTrack = 42;
    harness.control().trackDetail              = TrackDetail{};

    harness.send("GET", "/api/v1/nowplaying");

    // Both reads happen inside a single gate call. Two would let a track change
    // land between them, and the id from the first would miss in the second --
    // "playing, no track", which the player is never actually in.
    CHECK(harness.control().countOf("status") == 1);
    CHECK(harness.control().countOf("track") == 1);
}

TEST_CASE("nothing playing is a null track, not a 404", "[remote]") {
    Harness harness;
    harness.control().statusValue.currentTrack = kInvalidTrackId;

    const RawResponse response = harness.send("GET", "/api/v1/nowplaying");

    // A display polling this once a second should not have to treat the ordinary
    // idle case as an error.
    REQUIRE(response.status == 200);
    const nlohmann::json body = Harness::parse(response);
    CHECK(body.at("track").is_null());
    CHECK_FALSE(body.at("playing").get<bool>());
    // And the track was never asked for, because there was none to ask about.
    CHECK(harness.control().countOf("track") == 0);
}

TEST_CASE("the playlist is paged, and says how many matched", "[remote]") {
    Harness harness;
    for (TrackId id = 1; id <= 5; ++id) {
        harness.control().trackList.push_back(summary(id, "t" + std::to_string(id)));
    }

    const nlohmann::json body =
        Harness::parse(harness.send("GET", "/api/v1/playlist", {}, "offset=1&limit=2"));

    CHECK(body.at("total").get<std::size_t>() == 5);
    CHECK(body.at("offset").get<std::size_t>() == 1);
    REQUIRE(body.at("items").size() == 2);
    CHECK(body.at("items")[0].at("id").get<TrackId>() == 2);
}

TEST_CASE("the page size is capped", "[remote]") {
    Harness harness;
    // A client asking for everything on a hundred-thousand-row playlist would
    // hold the interface thread for the whole serialisation.
    const nlohmann::json body =
        Harness::parse(harness.send("GET", "/api/v1/playlist", {}, "limit=999999"));
    CHECK(body.at("limit").get<std::size_t>() == 1000);
}

TEST_CASE("a query string that is not numbers is 400", "[remote]") {
    Harness harness;
    CHECK(harness.send("GET", "/api/v1/playlist", {}, "offset=soon").status == 400);
    CHECK(harness.send("GET", "/api/v1/playlist", {}, "limit=-3").status == 400);
}

TEST_CASE("the filter is percent-decoded and passed through", "[remote]") {
    Harness harness;
    harness.send("GET", "/api/v1/playlist", {}, "q=Miles%20Davis");

    bool sawIt = false;
    for (const auto& call : harness.control().calls()) {
        if (call.name == "tracks" && call.text == "Miles Davis") {
            sawIt = true;
        }
    }
    CHECK(sawIt);
}

TEST_CASE("a plus in a query is a space", "[remote]") {
    Harness harness;
    // Not RFC 3986, but what every form and client library sends.
    harness.send("GET", "/api/v1/playlist", {}, "q=Miles+Davis");

    bool sawIt = false;
    for (const auto& call : harness.control().calls()) {
        if (call.name == "tracks" && call.text == "Miles Davis") {
            sawIt = true;
        }
    }
    CHECK(sawIt);
}

TEST_CASE("one track is served, and a missing one is 404", "[remote]") {
    Harness harness;

    TrackDetail detail;
    detail.summary = summary(7, "Blue in Green");
    detail.genre   = "Jazz";
    detail.metadata.emplace_back("ENGINEER", "Fraboni");
    harness.control().trackDetail = detail;

    const nlohmann::json body = Harness::parse(harness.send("GET", "/api/v1/playlist/7"));
    CHECK(body.at("title").get<std::string>() == "Blue in Green");
    CHECK(body.at("genre").get<std::string>() == "Jazz");
    // Unpromoted tags survive: the detail view is where a client goes for what
    // the list does not carry.
    CHECK(body.at("metadata").at("ENGINEER").get<std::string>() == "Fraboni");

    harness.control().trackDetail.reset();
    CHECK(harness.send("GET", "/api/v1/playlist/8").status == 404);
}

TEST_CASE("a track id that is not a number is 400", "[remote]") {
    Harness harness;
    CHECK(harness.send("GET", "/api/v1/playlist/seven").status == 400);
}

TEST_CASE("artwork is served with a sniffed type, or 404", "[remote]") {
    Harness harness;

    // A JPEG's first two bytes.
    auto jpeg = std::make_shared<std::vector<std::byte>>(
        std::vector<std::byte>{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}});
    harness.control().artworkValue = jpeg;

    const RawResponse response = harness.send("GET", "/api/v1/playlist/7/artwork");
    REQUIRE(response.status == 200);
    CHECK(response.contentType == "image/jpeg");
    CHECK(response.body.size() == 3);

    harness.control().artworkValue.reset();
    CHECK(harness.send("GET", "/api/v1/playlist/7/artwork").status == 404);
}

TEST_CASE("adding tracks answers 202 and a job to follow", "[remote]") {
    Harness harness;

    const RawResponse response = harness.send(
        "POST", "/api/v1/playlist/tracks", R"({"urls":["file:///a.flac"],"at":3})");

    // Not 200: a directory of ten thousand files would time out at the gate long
    // before the scan finished.
    REQUIRE(response.status == 202);
    CHECK(Harness::parse(response).at("jobId").get<std::string>() == "job-1");
    CHECK(harness.header(response, "Location") == "/api/v1/jobs/job-1");
}

TEST_CASE("adding nothing is a client error", "[remote]") {
    Harness harness;
    CHECK(harness.send("POST", "/api/v1/playlist/tracks", R"({"urls":[]})").status == 400);
    CHECK(harness.send("POST", "/api/v1/playlist/tracks", R"({"urls":"a"})").status == 400);
    CHECK(harness.send("POST", "/api/v1/playlist/tracks", R"({"urls":[1]})").status == 400);
    CHECK(harness.send("POST", "/api/v1/playlist/tracks", "{}").status == 400);
}

TEST_CASE("a host that cannot scan says so rather than pretending", "[remote]") {
    Harness harness;
    harness.control().jobId = "";

    const RawResponse response =
        harness.send("POST", "/api/v1/playlist/tracks", R"({"urls":["file:///a.flac"]})");
    CHECK(response.status == 501);
}

TEST_CASE("removing tracks needs a non-empty id list", "[remote]") {
    Harness harness;

    CHECK(harness.send("DELETE", "/api/v1/playlist/tracks", R"({"ids":[1,2]})").status == 200);
    CHECK(harness.control().countOf("removeTracks") == 1);

    // An id list that silently became empty would be a delete that quietly did
    // nothing and reported success.
    CHECK(harness.send("DELETE", "/api/v1/playlist/tracks", R"({"ids":[]})").status == 400);
    CHECK(harness.send("DELETE", "/api/v1/playlist/tracks", R"({"ids":["a"]})").status == 400);
    CHECK(harness.send("DELETE", "/api/v1/playlist/tracks", "{}").status == 400);
    CHECK(harness.control().countOf("removeTracks") == 1);
}

TEST_CASE("moving before null means the end", "[remote]") {
    Harness harness;

    CHECK(harness.send("POST", "/api/v1/playlist/move",
                       R"({"ids":[1],"before":null})").status == 200);
    CHECK(harness.send("POST", "/api/v1/playlist/move",
                       R"({"ids":[1],"before":4})").status == 200);

    const auto calls = harness.control().calls();
    bool       sawEnd = false;
    bool       sawAnchor = false;
    for (const auto& call : calls) {
        if (call.name == "moveTracks" && call.number == 0.0) {
            sawEnd = true;
        }
        if (call.name == "moveTracks" && call.number == 4.0) {
            sawAnchor = true;
        }
    }
    CHECK(sawEnd);
    CHECK(sawAnchor);
}

TEST_CASE("queueing defaults to queueing", "[remote]") {
    Harness harness;

    harness.send("POST", "/api/v1/playlist/queue", R"({"ids":[1]})");
    harness.send("POST", "/api/v1/playlist/queue", R"({"ids":[2],"queued":false})");

    const auto calls = harness.control().calls();
    bool on = false;
    bool off = false;
    for (const auto& call : calls) {
        if (call.name == "setQueued" && call.number == 1.0) { on = true; }
        if (call.name == "setQueued" && call.number == 0.0) { off = true; }
    }
    CHECK(on);
    CHECK(off);
}

TEST_CASE("patching a track takes one flag at a time or several", "[remote]") {
    Harness harness;
    harness.control().trackDetail = TrackDetail{};

    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"stopAfter":true})").status == 200);
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"rating":4})").status == 200);
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"rating":null})").status == 200);
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"playCount":0})").status == 200);

    // Out of range, and the one number a play count may be set to.
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"rating":9})").status == 400);
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", R"({"playCount":50})").status == 400);
    // Nothing at all says nothing.
    CHECK(harness.send("PATCH", "/api/v1/playlist/7", "{}").status == 400);
}

TEST_CASE("a job is reported, and an unknown one is 404", "[remote]") {
    Harness harness;

    JobStatus job;
    job.id    = "job-1";
    job.state = "running";
    job.done  = 41;
    job.total = 900;
    harness.control().jobValue = job;

    const nlohmann::json body = Harness::parse(harness.send("GET", "/api/v1/jobs/job-1"));
    CHECK(body.at("state").get<std::string>() == "running");
    CHECK(body.at("progress").at("done").get<int>() == 41);
    CHECK(body.at("error").is_null());

    harness.control().jobValue.reset();
    CHECK(harness.send("GET", "/api/v1/jobs/nope").status == 404);
}

TEST_CASE("undo and redo are reachable from the API", "[remote]") {
    Harness harness;
    // The same stack the Edit menu drives -- which is the point of routing edits
    // through PlaylistCommands rather than mutating the playlist directly.
    CHECK(harness.send("POST", "/api/v1/playlist/undo").status == 200);
    CHECK(harness.send("POST", "/api/v1/playlist/redo").status == 200);
    CHECK(harness.control().countOf("undo") == 1);
    CHECK(harness.control().countOf("redo") == 1);
}

// --- settings and DSP -------------------------------------------------------

TEST_CASE("settings are listed with their timing", "[remote]") {
    Harness harness;

    SettingInfo eq;
    eq.key         = "eq1kHz";
    eq.type        = "double";
    eq.value       = "0";
    eq.appliesFrom = "immediately";

    SettingInfo gain;
    gain.key         = "volumeScaling";
    gain.type        = "std::string";
    gain.value       = "albumGainWithPeak";
    gain.appliesFrom = "nextTrack";

    harness.control().settingList = {eq, gain};

    const nlohmann::json body = Harness::parse(harness.send("GET", "/api/v1/settings"));
    REQUIRE(body.at("items").size() == 2);
    // The pair that must not be reported alike: an equaliser band moves what is
    // playing and a ReplayGain mode cannot.
    CHECK(body.at("items")[0].at("appliesFrom").get<std::string>() == "immediately");
    CHECK(body.at("items")[1].at("appliesFrom").get<std::string>() == "nextTrack");
}

TEST_CASE("one setting is served, and an unknown key is 404", "[remote]") {
    Harness harness;

    SettingInfo info;
    info.key   = "enableFading";
    info.value = "true";
    harness.control().settingValue = info;

    CHECK(Harness::parse(harness.send("GET", "/api/v1/settings/enableFading"))
              .at("value")
              .get<std::string>() == "true");

    harness.control().settingValue.reset();
    CHECK(harness.send("GET", "/api/v1/settings/nosuchkey").status == 404);
}

TEST_CASE("a setting is written as text whatever its type", "[remote]") {
    Harness harness;
    harness.control().settingWrite = {Outcome::Ok, "immediately", {}};

    const RawResponse response =
        harness.send("PUT", "/api/v1/settings/enableFading", R"({"value":"true"})");
    REQUIRE(response.status == 200);
    CHECK(Harness::parse(response).at("appliesFrom").get<std::string>() == "immediately");

    // A JSON boolean for a bool setting would make the wire shape depend on a
    // type the client has to look up first.
    CHECK(harness.send("PUT", "/api/v1/settings/enableFading",
                       R"({"value":true})").status == 400);
    CHECK(harness.send("PUT", "/api/v1/settings/enableFading", "{}").status == 400);
}

TEST_CASE("session state is readable and not writable", "[remote]") {
    Harness harness;
    harness.control().settingWrite = {Outcome::ReadOnly, {},
                                      "That is session state."};

    const RawResponse response =
        harness.send("PUT", "/api/v1/settings/lastPlaybackStatus", R"({"value":"2"})");

    REQUIRE(response.status == 403);
    const nlohmann::json body = Harness::parse(response);
    CHECK(body.at("error").at("code").get<std::string>() == "read_only");
    // The reason the player gave, rather than the generic one.
    CHECK(body.at("error").at("message").get<std::string>() == "That is session state.");
}

TEST_CASE("a batch of settings is one hop and reports per key", "[remote]") {
    Harness harness;
    harness.control().settingWrite = {Outcome::Ok, "immediately", {}};

    const RawResponse response = harness.send(
        "PATCH", "/api/v1/settings", R"({"enableFading":"true","eqPreamp":"-3"})");

    REQUIRE(response.status == 200);
    CHECK(Harness::parse(response).at("results").size() == 2);
    CHECK(harness.control().countOf("setSetting") == 2);
}

TEST_CASE("a batch where one key was refused is 207, not 200 or 400", "[remote]") {
    Harness harness;
    harness.control().settingWrite = {Outcome::ReadOnly, {}, "no"};

    const RawResponse response =
        harness.send("PATCH", "/api/v1/settings", R"({"miniMode":"1"})");

    // Neither a success nor a failure: some keys took and some did not, and
    // flattening that either way loses which.
    CHECK(response.status == 207);
}

TEST_CASE("a batch with a non-string value names the key", "[remote]") {
    Harness harness;
    const RawResponse response =
        harness.send("PATCH", "/api/v1/settings", R"({"eqPreamp":-3})");
    REQUIRE(response.status == 400);
    CHECK(Harness::parse(response).at("error").at("field").get<std::string>() == "eqPreamp");
}

TEST_CASE("the equaliser reports its bands and says it applies at once", "[remote]") {
    Harness harness;
    harness.control().equalizerState.enabled = true;
    harness.control().equalizerState.preamp  = -3.0;
    harness.control().equalizerState.bands   = {{32.0, -1.5}, {64.0, 0.5}};

    const nlohmann::json body =
        Harness::parse(harness.send("GET", "/api/v1/dsp/equalizer"));

    CHECK(body.at("enabled").get<bool>());
    CHECK(body.at("preamp").get<double>() == -3.0);
    REQUIRE(body.at("bands").size() == 2);
    CHECK(body.at("bands")[0].at("hz").get<double>() == 32.0);
    CHECK(body.at("appliesFrom").get<std::string>() == "immediately");
}

TEST_CASE("the equaliser takes any of its three, and needs one", "[remote]") {
    Harness harness;

    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer", R"({"enabled":true})").status == 200);
    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer", R"({"preamp":-3})").status == 200);
    // 31.5, not 32. The bands are a fixed table and this is the first one that
    // is not a round number, so it is the one a client is most likely to guess
    // wrong.
    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer",
                       R"({"bands":[{"hz":31.5,"gain":-1.5}]})").status == 200);

    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer", "{}").status == 400);
    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer", R"({"bands":"loud"})").status == 400);
    // A band missing half of itself is not a band.
    CHECK(harness.send("PUT", "/api/v1/dsp/equalizer",
                       R"({"bands":[{"hz":31.5}]})").status == 400);
}

TEST_CASE("a frequency that is not a band says so, and says where to look",
          "[remote]") {
    Harness harness;

    // 32 is the plausible guess for the band that is actually at 31.5. Answering
    // "the player refused that value" and stopping there leaves a client author
    // with nothing to go on, so the route checks the frequency itself and names
    // it.
    const RawResponse response = harness.send(
        "PUT", "/api/v1/dsp/equalizer", R"({"bands":[{"hz":32,"gain":6}]})");

    REQUIRE(response.status == 400);
    const nlohmann::json body = Harness::parse(response);
    CHECK(body.at("error").at("code").get<std::string>() == "unknown_band");
    CHECK(body.at("error").at("field").get<std::string>() == "bands");
    CHECK(body.at("error").at("message").get<std::string>().find("32") !=
          std::string::npos);

    // Refused before it reached the player, so a request naming one good band
    // and one bad one leaves the curve alone rather than half-applied.
    CHECK(harness.control().countOf("setEqualizer") == 0);
}

TEST_CASE("presets are listed and applied", "[remote]") {
    Harness harness;
    harness.control().presetList = {"Flat", "Rock"};

    CHECK(Harness::parse(harness.send("GET", "/api/v1/dsp/equalizer/presets"))
              .at("items")
              .size() == 2);

    CHECK(harness.send("POST", "/api/v1/dsp/equalizer/preset",
                       R"({"name":"Rock"})").status == 200);
    CHECK(harness.send("POST", "/api/v1/dsp/equalizer/preset", R"({"name":""})").status == 400);

    harness.control().outcome = Outcome::NotFound;
    CHECK(harness.send("POST", "/api/v1/dsp/equalizer/preset",
                       R"({"name":"Nope"})").status == 404);
}

// --- the docs assets --------------------------------------------------------

TEST_CASE("the docs page needs no token, and the specification does", "[remote]") {
    Harness harness;

    // Forced rather than chosen: a browser cannot put an Authorization header on
    // a top-level navigation, so a token-gated /docs is a page nobody can open.
    // The page and everything it loads, or the browser gets a page it cannot
    // run. docs.js is ours and is on this list for the same reason the bundle
    // is: `script-src 'self'` means it has to come from here.
    for (const char* path : {"/docs", "/docs/swagger-ui.css",
                             "/docs/swagger-ui-bundle.js", "/docs/docs.js"}) {
        RawRequest anonymous;
        anonymous.method = "GET";
        anonymous.path   = path;
        INFO("path: " << path);
        CHECK(harness.handleRaw(anonymous).status == 200);
    }

    // What it describes is still behind the token, or /docs would be a way to
    // read the whole API surface without one.
    RawRequest spec;
    spec.method = "GET";
    spec.path   = "/openapi.json";
    CHECK(harness.handleRaw(spec).status == 401);
}

TEST_CASE("the docs assets are gzipped only for a client that said so",
          "[remote]") {
    Harness harness;

    const RawResponse compressed =
        harness.sendAccepting("/docs/swagger-ui-bundle.js", "gzip, deflate");
    CHECK(compressed.status == 200);
    CHECK(Harness::header(compressed, "Content-Encoding") == "gzip");

    // No Accept-Encoding at all. Sending gzip anyway is a body the client cannot
    // read -- cpp-httplib's own client, built without zlib, fails such a
    // response outright -- so it is expanded instead.
    const RawResponse plain = harness.sendAccepting("/docs/swagger-ui-bundle.js", "");
    CHECK(plain.status == 200);
    CHECK(Harness::header(plain, "Content-Encoding").empty());
    CHECK(plain.body.size() > compressed.body.size());
    // Really the script, not merely bigger.
    CHECK(plain.body.find("swagger") != std::string::npos);
}

TEST_CASE("the docs page carries a policy that pins it to this server",
          "[remote]") {
    Harness           harness;
    const RawResponse response = harness.sendAccepting("/docs", "gzip");

    const std::string policy = Harness::header(response, "Content-Security-Policy");
    CHECK(policy.find("default-src 'none'") != std::string::npos);
    CHECK(policy.find("connect-src 'self'") != std::string::npos);
    // What limits the damage if the vendored bundle were ever compromised: it
    // can reach nothing but this server.
    CHECK(policy.find("form-action 'none'") != std::string::npos);
}

#endif  // XPCOG_HAS_REST
