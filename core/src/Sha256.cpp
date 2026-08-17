#include "xpcog/core/Sha256.hpp"

#include <cstring>

namespace xpcog {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t major = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + major;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

Sha256Digest sha256(std::span<const std::byte> data) {
    std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                          0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                          0x1f83d9abU, 0x5be0cd19U};

    const auto*       bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t size  = data.size();

    std::size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) {
        compress(state, bytes + offset);
    }

    // Tail: the remainder, a 0x80 byte, zero padding, and the bit length. Two
    // blocks are needed when the remainder leaves no room for the length.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t             remaining = size - offset;
    if (remaining > 0) {
        std::memcpy(tail.data(), bytes + offset, remaining);
    }
    tail[remaining] = 0x80;

    const std::size_t tailBlocks = (remaining >= 56) ? 2 : 1;
    const std::size_t tailSize   = tailBlocks * 64;

    const std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bitLength >> (8 * i));
    }

    for (std::size_t block = 0; block < tailBlocks; ++block) {
        compress(state, tail.data() + block * 64);
    }

    Sha256Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

std::string sha256Hex(std::span<const std::byte> data) {
    static constexpr char kHex[] = "0123456789abcdef";

    const Sha256Digest digest = sha256(data);
    std::string        text;
    text.reserve(64);
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0F]);
    }
    return text;
}

}  // namespace xpcog
