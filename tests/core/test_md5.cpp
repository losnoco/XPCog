// MD5, against RFC 1321's own test suite.
//
// The seven vectors in the RFC's appendix A.5 are the whole of this file's
// reason to exist, and they are worth having in full rather than a couple of
// them: between them they cover the empty input, inputs shorter than a block,
// the boundary where the length no longer fits beside the padding in one block
// (the 56-byte case), and one longer than a block.
//
// Reference implementations are easy to get almost right. The two mistakes this
// pins are the ones a reader would make copying the SHA-256 next door: that MD5
// is little-endian in both the message schedule and the digest, where SHA-256 is
// big-endian in both.

#include "xpcog/core/Md5.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>

using namespace xpcog;

TEST_CASE("MD5 matches RFC 1321's test suite", "[md5]") {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5Hex("123456789012345678901234567890123456789012345678901234567890"
                 "12345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("MD5 handles the padding boundaries", "[md5]") {
    // 55 bytes: the largest input whose 0x80 byte and 8-byte length still fit in
    // one 64-byte block. 56 is the first that needs a second block. Getting the
    // comparison the wrong way round produces a digest that is correct for every
    // input except these two.
    const std::string fiftyFive(55, 'x');
    const std::string fiftySix(56, 'x');
    const std::string sixtyFour(64, 'x');

    CHECK(md5Hex(fiftyFive) == "04364420e25c512fd958a70738aa8f72");
    CHECK(md5Hex(fiftySix) == "668a72d5ba17f08e62dabcafad6db14b");
    CHECK(md5Hex(sixtyFour) == "c1bb4f81d892b2d57947682aeb252456");
}

TEST_CASE("MD5 hashes bytes as well as text", "[md5]") {
    // The span overload is what a caller with binary data uses; it must agree
    // with the string one on the same bytes.
    const std::string        text = "abc";
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(text.data()), text.size()};

    CHECK(md5Hex(bytes) == md5Hex(text));

    const Md5Digest digest = md5(bytes);
    CHECK(digest.size() == 16);
    CHECK(digest[0] == 0x90);
    CHECK(digest[15] == 0x72);
}
