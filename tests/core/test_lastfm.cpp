// The Last.fm protocol layer, against a fake transport.
//
// No network, on any CI job. Everything Last.fm's server would decide is decided
// here by handing the client a canned reply, which is the only way to test the
// interesting half: what the client does with error 14 as opposed to error 9 is
// a branch nobody can exercise against the real service on demand.
//
// The signature is pinned directly rather than through a round trip. It is the
// one thing here that is either exactly right or completely wrong, and the
// server's complaint about a wrong one -- error 13, "invalid method signature"
// -- names no parameter and gives no hint. Expected digests were computed with
// Python's hashlib against the concatenation Last.fm's specification describes.

#include "../FakeHttp.hpp"

#include "xpcog/core/Md5.hpp"
#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/LastFmClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <vector>

using namespace xpcog;
using xpcog::test::FakeHttp;

namespace {

constexpr std::string_view kKey    = "KEY";
constexpr std::string_view kSecret = "SECRET";

[[nodiscard]] ScrobbleTrack sampleTrack() {
    ScrobbleTrack track;
    track.artist    = "Boards of Canada";
    track.title     = "Roygbiv";
    track.album     = "Music Has the Right to Children";
    track.duration  = 152.0;
    track.startedAt = 1700000000;
    return track;
}

}  // namespace

// --- signing -------------------------------------------------------------

TEST_CASE("The signature sorts by name and skips format", "[lastfm]") {
    // Deliberately out of order, and carrying the one parameter that must not be
    // signed. Sorted this is a=1, b=2, and `format` is dropped, so the signed
    // string is "a1b2" + the secret.
    const HttpParams params = {{"b", "2"}, {"format", "json"}, {"a", "1"}};

    CHECK(LastFmClient::signature(params, "sec") == "35ba30f61c05c14be609e84509030ec5");
    CHECK(LastFmClient::signature(params, "sec") == md5Hex("a1b2sec"));
}

TEST_CASE("A signed call carries the parameters Last.fm expects", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"token":"TOK123"})");
    const auto token = client.requestToken();
    REQUIRE(token);
    CHECK(*token == "TOK123");

    REQUIRE(http.callCount() == 1);
    CHECK(http.calls()[0].url == "https://ws.audioscrobbler.com/2.0/");
    // getToken is a GET, as Last.fm's own example has it.
    CHECK(http.calls()[0].post == false);

    CHECK(http.sent(0, "method") == "auth.getToken");
    CHECK(http.sent(0, "api_key") == std::string{kKey});
    CHECK(http.sent(0, "format") == "json");
    // Signed over api_key and method only, in that sorted order.
    CHECK(http.sent(0, "api_sig") == "66ee63a18da3c919f987b342697d913c");

    // No session key on an unauthenticated call: sending `sk` here would change
    // the signature and be rejected.
    CHECK(http.sent(0, "sk") == std::nullopt);
}

// --- the desktop authentication flow ------------------------------------

TEST_CASE("The authorisation URL carries the key and the token", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    const std::string url = client.authorizationUrl("TOK123");
    CHECK(url == "https://www.last.fm/api/auth/?api_key=KEY&token=TOK123");

    // No secret in it. This URL goes to a browser, into history, and possibly
    // into a bug report.
    CHECK(url.find(std::string{kSecret}) == std::string::npos);
}

TEST_CASE("An ungranted token reports NotAuthorized rather than failing",
          "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    // Error 14 is the expected state while the listener is still looking at the
    // browser, so it has to be distinguishable from a real failure -- the pane
    // keeps waiting on this one and gives up on the others.
    http.reply(200,
               R"({"error":14,"message":"This token has not been authorized"})");

    LastFmError error;
    const auto  session = client.session("TOK123", &error);
    CHECK(!session);
    CHECK(error.kind == LastFmError::Kind::NotAuthorized);
    CHECK(error.code == 14);

    CHECK(http.sent(0, "method") == "auth.getSession");
    CHECK(http.sent(0, "token") == "TOK123");
    CHECK(http.sent(0, "api_sig") == "bfe83dac5cee3cc558d3b510cd53c72c");
}

TEST_CASE("A granted token yields a session key and a username", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200,
               R"({"session":{"name":"listener","key":"SESSIONKEY","subscriber":0}})");

    LastFmError error;
    const auto  session = client.session("TOK123", &error);
    REQUIRE(session);
    CHECK(session->key == "SESSIONKEY");
    CHECK(session->username == "listener");
    CHECK(error.ok());
}

TEST_CASE("An expired token is not retried", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"error":15,"message":"This token has expired"})");

    LastFmError error;
    CHECK(!client.session("TOK123", &error));
    CHECK(error.kind == LastFmError::Kind::Api);
    CHECK(error.code == 15);
    // Waiting longer cannot un-expire it; the flow has to start again.
    CHECK(error.retryable() == false);
}

// --- scrobbling ----------------------------------------------------------

TEST_CASE("A scrobble is posted with its timestamp", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"scrobbles":{"@attr":{"accepted":1,"ignored":0}}})");

    const ScrobbleTrack        track = sampleTrack();
    const std::vector<ScrobbleTrack> batch{track};

    LastFmError error;
    const auto  result = client.scrobble(batch, "SESSIONKEY", &error);
    REQUIRE(result);
    CHECK(result->accepted == 1);
    CHECK(result->ignored == 0);

    REQUIRE(http.callCount() == 1);
    CHECK(http.calls()[0].post == true);
    CHECK(http.sent(0, "method") == "track.scrobble");
    CHECK(http.sent(0, "sk") == "SESSIONKEY");
    // Indexed even for one entry, which is the form that also works for fifty.
    CHECK(http.sent(0, "artist[0]") == "Boards of Canada");
    CHECK(http.sent(0, "track[0]") == "Roygbiv");
    CHECK(http.sent(0, "timestamp[0]") == "1700000000");
    CHECK(http.sent(0, "duration[0]") == "152");
}

TEST_CASE("A batch indexes every entry", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"scrobbles":{"@attr":{"accepted":3,"ignored":0}}})");

    std::vector<ScrobbleTrack> batch;
    for (int i = 0; i < 3; ++i) {
        ScrobbleTrack track = sampleTrack();
        track.title         = "Track " + std::to_string(i);
        track.startedAt     = 1700000000 + i * 200;
        batch.push_back(track);
    }

    REQUIRE(client.scrobble(batch, "SESSIONKEY"));
    CHECK(http.sent(0, "track[0]") == "Track 0");
    CHECK(http.sent(0, "track[2]") == "Track 2");
    CHECK(http.sent(0, "timestamp[2]") == "1700000400");
}

TEST_CASE("A batch larger than fifty is refused rather than truncated",
          "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    // Truncating would silently drop the tail, which for a queue means losing
    // scrobbles with no error anywhere.
    const std::vector<ScrobbleTrack> batch(51, sampleTrack());
    CHECK(!client.scrobble(batch, "SESSIONKEY"));
    CHECK(http.callCount() == 0);
}

TEST_CASE("An album artist equal to the artist is not sent", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"scrobbles":{"@attr":{"accepted":1,"ignored":0}}})");

    ScrobbleTrack track = sampleTrack();
    track.albumArtist   = track.artist;
    const std::vector<ScrobbleTrack> batch{track};
    REQUIRE(client.scrobble(batch, "SESSIONKEY"));

    // Cog's rule: sending it identical makes a single-artist album look like a
    // compilation on the listener's profile.
    CHECK(http.sent(0, "albumArtist[0]") == std::nullopt);
}

TEST_CASE("A differing album artist is sent", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"scrobbles":{"@attr":{"accepted":1,"ignored":0}}})");

    ScrobbleTrack track = sampleTrack();
    track.albumArtist   = "Various Artists";
    const std::vector<ScrobbleTrack> batch{track};
    REQUIRE(client.scrobble(batch, "SESSIONKEY"));

    CHECK(http.sent(0, "albumArtist[0]") == "Various Artists");
}

TEST_CASE("A partly ignored batch reports what happened", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    // 200 with an ignored entry. Reading only the status here would report
    // success for a scrobble that never appeared on the listener's profile.
    http.reply(200, R"({"scrobbles":{"@attr":{"accepted":1,"ignored":1},
        "scrobble":[{"ignoredMessage":{"code":"1","#text":"Artist was ignored"}},
                    {"ignoredMessage":{"code":"0","#text":""}}]}})");

    const std::vector<ScrobbleTrack> batch(2, sampleTrack());
    const auto result = client.scrobble(batch, "SESSIONKEY");
    REQUIRE(result);
    CHECK(result->accepted == 1);
    CHECK(result->ignored == 1);
    CHECK(result->ignoredReason == "Artist was ignored");
}

TEST_CASE("Now-playing carries no timestamp", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"nowplaying":{"track":{"#text":"Roygbiv"}}})");
    CHECK(client.updateNowPlaying(sampleTrack(), "SESSIONKEY"));

    CHECK(http.sent(0, "method") == "track.updateNowPlaying");
    CHECK(http.sent(0, "artist") == "Boards of Canada");
    // Bare names, not indexed, and no timestamp: this call is about the present.
    CHECK(http.sent(0, "timestamp") == std::nullopt);
    CHECK(http.sent(0, "artist[0]") == std::nullopt);
}

TEST_CASE("A track with no artist is refused before it is sent", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    ScrobbleTrack track = sampleTrack();
    track.artist.clear();

    CHECK(!client.updateNowPlaying(track, "SESSIONKEY"));
    CHECK(http.callCount() == 0);
}

// --- errors --------------------------------------------------------------

TEST_CASE("An invalid session key is its own kind", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.reply(200, R"({"error":9,"message":"Invalid session key"})");

    const std::vector<ScrobbleTrack> batch{sampleTrack()};
    LastFmError                      error;
    CHECK(!client.scrobble(batch, "STALE", &error));

    // The one error whose correct handling is to discard credentials, so it must
    // not be lost among the generic ones.
    CHECK(error.kind == LastFmError::Kind::SessionInvalid);
    CHECK(error.retryable() == false);
}

TEST_CASE("Temporary failures are retryable and permanent ones are not",
          "[lastfm]") {
    const auto apiError = [](int code) {
        LastFmError error;
        error.kind = LastFmError::Kind::Api;
        error.code = code;
        return error;
    };

    // Worth retrying: the service is unwell or we are being throttled.
    CHECK(apiError(8).retryable());   // operation failed
    CHECK(apiError(11).retryable());  // service offline
    CHECK(apiError(16).retryable());  // temporary error
    CHECK(apiError(29).retryable());  // rate limit

    // Not worth retrying: the request itself is wrong and will be tomorrow too.
    CHECK_FALSE(apiError(4).retryable());   // authentication failed
    CHECK_FALSE(apiError(10).retryable());  // invalid api key
    CHECK_FALSE(apiError(13).retryable());  // invalid signature
    CHECK_FALSE(apiError(26).retryable());  // suspended api key

    LastFmError transport;
    transport.kind = LastFmError::Kind::Transport;
    CHECK(transport.retryable());
}

TEST_CASE("A transport failure is not read as an API answer", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    http.failTransport("could not resolve host");

    LastFmError error;
    CHECK(!client.requestToken(&error));
    CHECK(error.kind == LastFmError::Kind::Transport);
    CHECK(error.message == "could not resolve host");
    CHECK(error.retryable());
}

TEST_CASE("An unreadable body is not mistaken for success", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, std::string{kKey}, std::string{kSecret}};

    // A captive portal answering 200 with a login page is the realistic way
    // here: status says fine, body is not ours.
    http.reply(200, "<html>Sign in to continue</html>");

    LastFmError error;
    CHECK(!client.requestToken(&error));
    CHECK(error.kind == LastFmError::Kind::Malformed);
}

TEST_CASE("A build with no API key never reaches the network", "[lastfm]") {
    FakeHttp     http;
    LastFmClient client{http, "", ""};

    CHECK(client.configured() == false);

    LastFmError error;
    CHECK(!client.requestToken(&error));
    CHECK(http.callCount() == 0);
}

// --- encoding ------------------------------------------------------------

TEST_CASE("Percent-encoding escapes what a form body would otherwise eat",
          "[lastfm]") {
    // A space is %20 rather than '+', because the same encoder builds query
    // strings, where '+' is a literal plus.
    CHECK(percentEncode("a b") == "a%20b");
    CHECK(percentEncode("Sigur Ros & Co") == "Sigur%20Ros%20%26%20Co");
    CHECK(percentEncode("a=b") == "a%3Db");
    // Unreserved characters survive untouched.
    CHECK(percentEncode("aZ0-_.~") == "aZ0-_.~");
    // Non-ASCII goes out as its UTF-8 bytes, escaped.
    CHECK(percentEncode("Björk") == "Bj%C3%B6rk");
}

TEST_CASE("The signature covers the unencoded value", "[lastfm]") {
    // Signing the encoded form is the classic way to earn error 13, so this
    // pins which of the two is hashed: an artist with a space signs as the
    // space, and is only escaped on the way out.
    const HttpParams params = {{"artist", "a b"}};
    CHECK(LastFmClient::signature(params, "s") == md5Hex("artista bs"));
    CHECK(formEncode(params) == "artist=a%20b");
}
