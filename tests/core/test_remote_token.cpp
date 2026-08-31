// The access token: where it comes from, and how it is checked.
//
// The generator cannot be tested for randomness in any meaningful sense, and
// this does not pretend to. What it pins is the shape -- the length and alphabet
// a client will be told to expect -- and the property that two calls do not
// agree, which is the failure a deterministic std::random_device would produce
// and which would otherwise ship silently.
//
// The comparison can only be tested for correctness. Whether it leaks timing is
// not something a unit test can answer on a machine with a scheduler on it; the
// reason it is written without an early exit is in the header, and this checks
// it still says yes and no in the right places.

#include "xpcog/core/remote/Token.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace xpcog::remote;

TEST_CASE("a token is 64 lowercase hex characters", "[remote]") {
    const std::string token = generateRemoteToken();

    REQUIRE(token.size() == 64);
    CHECK(std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
}

TEST_CASE("tokens do not repeat", "[remote]") {
    // The MinGW failure this guards against: std::random_device there returned
    // the same sequence on every run, so every installation shared a token. Not
    // a statistical test -- a collision in sixteen draws from 2^256 is not
    // something to reason about, it is something that means the generator is
    // broken.
    std::set<std::string> seen;
    for (int i = 0; i < 16; ++i) {
        seen.insert(generateRemoteToken());
    }
    CHECK(seen.size() == 16);
}

TEST_CASE("constant-time compare still says yes and no", "[remote]") {
    CHECK(constantTimeEquals("", ""));
    CHECK(constantTimeEquals("abc", "abc"));

    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "Abc"));
    // Differing in the first byte and in the last, which is the pair a
    // short-circuiting comparison would answer at different speeds.
    CHECK_FALSE(constantTimeEquals("xbc", "abc"));

    // Length is not a secret -- every token is 64 characters -- so a difference
    // in it is answered at once rather than padded.
    CHECK_FALSE(constantTimeEquals("abc", "abcd"));
    CHECK_FALSE(constantTimeEquals("", "a"));
}
