// The one question a fake transport cannot answer: does Last.fm accept our
// signature?
//
// Hidden, like the device cases in test_audio.cpp, and run the same way:
//
//     xpcog-tests "[.lastfmlive]"
//
// It needs a real API key, which it takes from the environment rather than from
// the build's baked-in one:
//
//     XPCOG_LASTFM_API_KEY=... XPCOG_LASTFM_API_SECRET=... xpcog-tests "[.lastfmlive]"
//
// From the environment for two reasons. A test binary that carried a shared
// secret in its .rdata is a secret in every build artefact and every CI cache;
// and reading it here means this case works against a build configured with no
// key at all, which is what a clean checkout is.
//
// **Why `auth.getToken` specifically.** It is the only call in the flow that is
// fully signed, reaches the real service, and needs no human: a request token is
// issued to anyone who asks correctly, and is useless until somebody grants it
// in a browser. So this exercises exactly the layer that has no other witness --
// the parameter set, the sorted concatenation, the MD5, and the percent-encoding
// on the way out -- and stops short of anything that would touch an account.
//
// Nothing is scrobbled, no session is created, and no listening history is
// touched. The token this obtains is discarded unheard of, and expires in an
// hour on its own.
//
// What a failure means, which is the reason this case is worth its file:
//
//   * **Error 13**, "invalid method signature supplied" -- the signature is
//     wrong, and the server will not say which parameter. Suspect the sorted
//     concatenation, whether `format` was included, or whether the encoded
//     rather than the raw value was signed.
//   * **Error 10**, "invalid API key" -- the key is not what the account holds.
//   * **Error 26**, "suspended API key" -- the key is real and has been
//     switched off.
//   * A transport failure -- this machine cannot reach the service, which says
//     nothing about the code.

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/LastFmClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace xpcog;

namespace {

[[nodiscard]] std::string fromEnvironment(const char* name) {
#ifdef _MSC_VER
    // getenv is deprecated under MSVC's secure-CRT warnings, and _dupenv_s is
    // the sanctioned form. The free() is why this is not a one-liner.
    char*       buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return (value != nullptr) ? std::string{value} : std::string{};
#endif
}

}  // namespace

TEST_CASE("Last.fm accepts a signature we generated", "[.lastfmlive]") {
    const std::string key    = fromEnvironment("XPCOG_LASTFM_API_KEY");
    const std::string secret = fromEnvironment("XPCOG_LASTFM_API_SECRET");

    if (key.empty() || secret.empty()) {
        SKIP("set XPCOG_LASTFM_API_KEY and XPCOG_LASTFM_API_SECRET to run this");
    }

    auto http = makeCurlHttpClient();
    if (!http) {
        SKIP("this build has no HTTP client");
    }

    LastFmClient client{*http, key, secret};
    REQUIRE(client.configured());

    LastFmError error;
    const auto  token = client.requestToken(&error);

    if (!token && error.kind == LastFmError::Kind::Transport) {
        // Not a verdict on the code. Said as a skip rather than a failure so a
        // machine behind a proxy does not report a signing bug it has no
        // evidence for.
        SKIP("could not reach Last.fm: " + error.message);
    }

    INFO("Last.fm error " << error.code << ": " << error.message);
    REQUIRE(token);

    // Non-empty, and made only of characters that survive a URL unescaped --
    // which matters because the next thing that happens to this token is being
    // pasted into a query string and opened in a browser.
    //
    // **Observed, not documented.** Last.fm's API reference does not specify the
    // token's format. Against the real service it is 32 characters of
    // base64url -- mixed case, digits, `-` and `_` -- and an earlier version of
    // this test asserted 32 lowercase hex characters because the API *key* and
    // *secret* are hex and that seemed to follow. It does not, and the service
    // said so. So this checks the property the code actually depends on rather
    // than a shape that happens to hold today.
    CHECK(!token->empty());
    for (const char c : *token) {
        const bool urlSafe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        CHECK(urlSafe);
    }

    // And the URL a listener would be sent to is well formed around it. This is
    // as far as the flow goes without a person: everything after this needs
    // somebody to press Allow.
    const std::string url = client.authorizationUrl(*token);
    CHECK(url.find("https://www.last.fm/api/auth/?api_key=") == 0);
    CHECK(url.find(*token) != std::string::npos);
    CHECK(url.find(secret) == std::string::npos);
}
