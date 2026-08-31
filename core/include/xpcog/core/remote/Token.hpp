// The remote control's access token: making one, and checking one.
//
// Public rather than private to core/src/remote because the application makes
// the token and core only ever checks it. Where it is kept is the app's problem
// too -- wxSecretStore, for the reason LastFmAccount.hpp already gives about
// where a credential does not go -- and core is handed the value in
// ServerConfig.
//
// Compiled whether or not XPCOG_WITH_REST is on. It names no HTTP library and
// costs nothing, and a preferences pane that greys itself in a build without a
// server should not also fail to link.

#pragma once

#include <string>
#include <string_view>

namespace xpcog::remote {

/// A new token: 32 bytes from the operating system's cryptographic generator,
/// as 64 lowercase hex characters.
///
/// **Not std::random_device.** The standard permits it to be a deterministic
/// engine, and libstdc++ on MinGW famously was one -- a token that is the same
/// on every machine is not a token. This asks BCryptGenRandom on Windows and
/// getentropy() elsewhere, falling back to /dev/urandom.
///
/// Hex rather than base64 for a reason that is small but real: the only base64
/// codec in this tree is private to the plist reader, and promoting it into a
/// public core header to save 21 characters would be a change to core's surface
/// paid for by nothing.
///
/// Empty when the system generator failed, which is not a case to paper over --
/// the caller must not fall back to something guessable, and the server refuses
/// to start without a token.
[[nodiscard]] std::string generateRemoteToken();

/// Compares two tokens without leaking where they first differ through timing.
///
/// The lengths are compared first and that is deliberate: both are 64 hex
/// characters, so the length is not a secret, and a comparison that ran to the
/// longer of the two would have to decide what to read past the end of the
/// shorter one. What must not vary is *where* two equal-length tokens diverge,
/// and the loop below has no early exit for that reason.
[[nodiscard]] bool constantTimeEquals(std::string_view a, std::string_view b) noexcept;

}  // namespace xpcog::remote
