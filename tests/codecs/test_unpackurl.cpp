// Addressing a file inside an archive.
//
// The format is Cog's, and the whole reason it has a length prefix is that both
// halves are arbitrary filesystem paths: either may contain the separator, so
// there is no character that could delimit them and counting is the only thing
// that can. That makes the awkward inputs the point of these tests rather than
// an afterthought.

#include "archive/UnpackUrl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace xpcog;
using xpcog::codecs::makeUnpackUrl;
using xpcog::codecs::parseUnpackUrl;
using xpcog::codecs::UnpackTarget;

TEST_CASE("an unpack URL round-trips", "[archive][unpack]") {
    const Url url = makeUnpackUrl("/music/soundtrack.zip", "03 Theme.mod");
    REQUIRE(!url.empty());
    CHECK(url.scheme() == "unpack");

    const auto target = parseUnpackUrl(url);
    REQUIRE(target.has_value());
    CHECK(target->archive == "/music/soundtrack.zip");
    CHECK(target->member == "03 Theme.mod");
}

TEST_CASE("the member's extension is what picks the decoder", "[archive][unpack]") {
    // Not the archive's. A .mod inside a .zip has to reach the module decoder,
    // and the registry chooses on Url::extension() -- so the encoding used when
    // building has to leave the member's dot and slashes legible.
    CHECK(makeUnpackUrl("/music/pack.zip", "song.mod").extension() == "mod");
    CHECK(makeUnpackUrl("/music/pack.7z", "sub/dir/tune.spc").extension() == "spc");
    CHECK(makeUnpackUrl("/music/RIP.RSN", "Track.SPC").extension() == "spc");
}

TEST_CASE("paths containing the separator survive", "[archive][unpack]") {
    // The case the length prefix exists for. A bar in either half would end the
    // parse early for anything that split on it.
    const Url url = makeUnpackUrl("/music/rock|pop.zip", "a|b|c.mod");

    const auto target = parseUnpackUrl(url);
    REQUIRE(target.has_value());
    CHECK(target->archive == "/music/rock|pop.zip");
    CHECK(target->member == "a|b|c.mod");
}

TEST_CASE("a hash in either path does not become a fragment", "[archive][unpack]") {
    // XPCog splits a URL on '#' to find the subsong fragment, and the fragment
    // has to stay free for the track number of an archived rip. A '#' in a
    // filename must therefore never reach the stored URL unencoded.
    const Url url = makeUnpackUrl("/music/#1 hits.zip", "track #2.mod");
    CHECK(url.fragment().empty());

    const auto target = parseUnpackUrl(url);
    REQUIRE(target.has_value());
    CHECK(target->archive == "/music/#1 hits.zip");
    CHECK(target->member == "track #2.mod");
}

TEST_CASE("a subsong fragment survives alongside the member", "[archive][unpack]") {
    // An archived NSF has both: which member of the archive, and which track of
    // the NSF. Keeping the member in the path is what leaves room for both.
    const Url base  = makeUnpackUrl("/music/rips.7z", "game.nsf");
    const Url track = base.withFragment("3");

    CHECK(track.fragment() == "3");
    const auto target = parseUnpackUrl(track);
    REQUIRE(target.has_value());
    CHECK(target->member == "game.nsf");
}

TEST_CASE("a URL Cog wrote parses here", "[archive][unpack]") {
    // Hand-built in Cog's exact shape rather than produced by makeUnpackUrl, so
    // this fails if the format drifts rather than agreeing with itself.
    // "/music/pack.zip" is 15 characters.
    const auto url = Url::parse("unpack://fex|15|/music/pack.zip|song.mod");
    REQUIRE(url.has_value());

    const auto target = parseUnpackUrl(*url);
    REQUIRE(target.has_value());
    CHECK(target->archive == "/music/pack.zip");
    CHECK(target->member == "song.mod");
}

TEST_CASE("malformed unpack URLs are refused rather than guessed at",
          "[archive][unpack]") {
    const auto refused = [](const char* text) {
        const auto url = Url::parse(text);
        return url.has_value() && !parseUnpackUrl(*url).has_value();
    };

    CHECK(refused("unpack://fex|/music/pack.zip|song.mod"));   // no length
    CHECK(refused("unpack://fex|nine|/music/pack.zip|song"));  // length not a number
    CHECK(refused("unpack://fex|999|/music/pack.zip|song"));   // length past the end
    CHECK(refused("unpack://fex|4|/music/pack.zip|song"));     // length misses the bar
    CHECK(refused("unpack://fex|15|/music/pack.zip|"));        // no member
    CHECK(refused("unpack://zzz|15|/music/pack.zip|song"));    // another extractor
    CHECK(refused("unpack://fex"));                            // nothing at all

    // And a URL that is simply not this scheme.
    const auto plain = Url::parse("file:///music/song.mod");
    REQUIRE(plain.has_value());
    CHECK(!parseUnpackUrl(*plain).has_value());
}
