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

TEST_CASE("the docs page is embedded and mentions what it needs", "[remote]") {
    const std::span<const std::byte> bytes = resources::swagger("index.html");
    REQUIRE_FALSE(bytes.empty());

    const std::string page{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // The page is ours and stays readable in the tree, so this checks meaning
    // rather than a hash that would have to be updated on every wording change.
    // It has to fetch the spec, and it has to attach the token itself -- a page
    // that did neither would load and then be unable to do anything.
    CHECK(page.find("/openapi.json") != std::string::npos);
    CHECK(page.find("requestInterceptor") != std::string::npos);
    CHECK(page.find("Bearer ") != std::string::npos);

    // And it loads its own copies rather than a CDN, which is the point of
    // vendoring them: a player on loopback has no internet to reach one.
    CHECK(page.find("/docs/swagger-ui-bundle.js") != std::string::npos);
    CHECK(page.find("http://cdn") == std::string::npos);
    CHECK(page.find("https://cdn") == std::string::npos);
}

TEST_CASE("an asset the build does not have answers empty", "[remote]") {
    // The embedder's contract, and the cue a caller relies on to answer 404
    // rather than serve nothing with a 200 on it.
    CHECK(resources::swagger("nothing-here.js").empty());
}
