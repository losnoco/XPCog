#include "xpcog/core/remote/Token.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#    include <windows.h>
//  After windows.h, which it needs.
#    include <bcrypt.h>
#else
//  getentropy is declared in <unistd.h> on OpenBSD and in <sys/random.h> on
//  glibc and on macOS -- Apple's man page is explicit about that, and getting it
//  wrong is a build that only fails on the platform you are not on. Asking
//  __has_include rather than enumerating platforms means the next libc does not
//  need a line here.
#    include <unistd.h>
#    if __has_include(<sys/random.h>)
#        include <sys/random.h>
#    endif
#    include <cstdio>
#endif

namespace xpcog::remote {
namespace {

constexpr std::size_t kTokenBytes = 32;

/// Fills `out` from the system's cryptographic generator. False when it would
/// not -- reported rather than worked around, because the only thing to fall
/// back to is something predictable.
bool systemRandom(std::byte* out, std::size_t length) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out),
                           static_cast<ULONG>(length),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    // getentropy() is capped at 256 bytes per call by contract; 32 is well
    // inside it, so the loop a larger request would need is not written here.
    if (getentropy(out, length) == 0) {
        return true;
    }
    // A kernel too old for getentropy, or a sandbox that refused it. The device
    // is the same source and is worth trying before giving up.
    std::FILE* device = std::fopen("/dev/urandom", "rb");
    if (device == nullptr) {
        return false;
    }
    const std::size_t read = std::fread(out, 1, length, device);
    std::fclose(device);
    return read == length;
#endif
}

}  // namespace

std::string generateRemoteToken() {
    std::array<std::byte, kTokenBytes> bytes{};
    if (!systemRandom(bytes.data(), bytes.size())) {
        return {};
    }

    constexpr char kDigits[] = "0123456789abcdef";
    std::string    token;
    token.reserve(kTokenBytes * 2);
    for (const std::byte byte : bytes) {
        const auto value = static_cast<std::uint8_t>(byte);
        token.push_back(kDigits[value >> 4U]);
        token.push_back(kDigits[value & 0x0FU]);
    }
    return token;
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    // volatile so the accumulation is not optimised into an early exit. The
    // whole point is that every byte is read whatever the first one said.
    volatile std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference = static_cast<std::uint8_t>(
            difference | (static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i])));
    }
    return difference == 0;
}

}  // namespace xpcog::remote
