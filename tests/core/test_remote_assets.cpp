// The vendored Swagger UI, and the check that it is still what was vendored.
//
// A committed minified blob is opaque to review: nobody reads 409 KB of
// generated JavaScript in a diff, so nothing about the ordinary process would
// notice it being replaced -- by accident, in a bad merge, or otherwise. The
// hashes in assets/swagger-ui/MANIFEST are what make a replacement loud, and
// this is what reads them.
//
// It hashes the *embedded* bytes rather than the files on disk, so it is testing
// what a built player would actually serve rather than what is sitting in the
// source tree beside it.

#include "swagger_resources.hpp"

#include "xpcog/core/Sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>

using namespace xpcog;

namespace {

/// The `committed` hashes out of MANIFEST, by file name.
std::map<std::string, std::string> manifestHashes() {
    std::map<std::string, std::string> hashes;

    std::ifstream file{std::string{XPCOG_SWAGGER_DIR} + "/MANIFEST"};
    REQUIRE(file.is_open());

    std::string line;
    std::string current;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream parts{line};
        std::string        first;
        parts >> first;
        if (first == "upstream") {
            continue;
        }
        if (first == "committed") {
            std::string hash;
            parts >> hash;
            if (!current.empty() && !hash.empty()) {
                hashes[current] = hash;
            }
            continue;
        }
        current = first;
    }
    return hashes;
}

std::string hashOf(std::span<const std::byte> bytes) {
    return sha256Hex(bytes);
}

}  // namespace

TEST_CASE("the embedded Swagger UI is the one MANIFEST describes", "[remote]") {
    const std::map<std::string, std::string> expected = manifestHashes();
    REQUIRE(expected.size() == 2);

    for (const char* name : {"swagger-ui.css.gz", "swagger-ui-bundle.js.gz"}) {
        INFO("asset: " << name);
        const std::span<const std::byte> bytes = resources::swagger(name);
        // Empty means the embedder does not have it, which is what a rename with
        // no CMake change looks like.
        REQUIRE_FALSE(bytes.empty());

        const auto found = expected.find(name);
        REQUIRE(found != expected.end());
        CHECK(hashOf(bytes) == found->second);
    }
}

TEST_CASE("the docs page is embedded and loads only its own files", "[remote]") {
    const std::span<const std::byte> bytes = resources::swagger("index.html");
    REQUIRE_FALSE(bytes.empty());

    const std::string page{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // Its own copies rather than a CDN, which is the point of vendoring them: a
    // player on loopback has no internet to reach one.
    CHECK(page.find("/docs/swagger-ui-bundle.js") != std::string::npos);
    CHECK(page.find("/docs/docs.js") != std::string::npos);
    CHECK(page.find("http://cdn") == std::string::npos);
    CHECK(page.find("https://cdn") == std::string::npos);
}

TEST_CASE("the docs page has no inline script", "[remote]") {
    const std::span<const std::byte> bytes = resources::swagger("index.html");
    REQUIRE_FALSE(bytes.empty());
    const std::string page{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // The page is served with `script-src 'self'`, which blocks inline execution
    // outright -- so an inline <script> is not a style question here, it is a
    // page that loads and then does nothing at all. This shipped once: the CSP
    // was asserted by its text and the page was never opened in a browser, so
    // nothing noticed that the two disagreed.
    //
    // Every <script> must therefore carry a src. Scanning for the ones that do
    // not is cruder than parsing the HTML and is the right size for the job:
    // there are three script tags and they are all in this file.
    std::size_t at = page.find("<script");
    while (at != std::string::npos) {
        const std::size_t end = page.find('>', at);
        REQUIRE(end != std::string::npos);
        const std::string tag = page.substr(at, end - at);
        INFO("tag: " << tag);
        CHECK(tag.find("src=") != std::string::npos);
        at = page.find("<script", end);
    }
}

TEST_CASE("the docs script fetches the spec and attaches the token", "[remote]") {
    const std::span<const std::byte> bytes = resources::swagger("docs.js");
    REQUIRE_FALSE(bytes.empty());

    const std::string script{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // Checked by meaning rather than by hash, because this file is ours and
    // stays readable: a hash would have to be updated on every wording change
    // and would say nothing about whether the page works. It has to fetch the
    // specification, and it has to attach the token itself -- a page that did
    // neither would load and then be unable to do anything.
    CHECK(script.find("/openapi.json") != std::string::npos);
    CHECK(script.find("requestInterceptor") != std::string::npos);
    CHECK(script.find("Bearer ") != std::string::npos);
}

TEST_CASE("an asset the build does not have answers empty", "[remote]") {
    // The embedder's contract, and the cue a caller relies on to answer 404
    // rather than serve nothing with a 200 on it.
    CHECK(resources::swagger("nothing-here.js").empty());
}
