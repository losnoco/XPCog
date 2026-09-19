// The Last.fm web API, as much of it as scrobbling needs.
//
// Port of Cog Scrobbler/LastFMAPI.swift, with one deliberate departure: the
// **authentication flow is the desktop one**, not the mobile one Cog uses.
//
// Cog calls `auth.getMobileSession`, which takes the listener's username and
// password and posts them. It works, and it saves a trip through a browser. The
// cost is that the application handles the account password at all -- it is
// typed into a text field that belongs to us, held in memory by us, and sent by
// us. Last.fm documents that method for mobile clients that cannot open a
// browser, and documents this one for desktop applications, which can.
//
// So the flow here is the three-step one:
//
//   1. `requestToken()`            -- ask for a request token
//   2. `authorizationUrl(token)`   -- the listener opens this and grants access
//   3. `session(token)`            -- exchange the granted token for a session key
//
// What that buys, concretely: no password ever reaches this process, the grant
// happens on a page whose address bar says last.fm, the listener sees exactly
// what is being authorised, and revoking it later is something they do on their
// own account page rather than by trusting us to forget. The session key that
// comes back does not expire and is not a password -- it authorises scrobbling
// and nothing else.
//
// What it costs is that step 2 is not ours to complete. The listener has to
// actually visit the page, so `session()` fails with `Kind::NotAuthorized`
// until they do -- which is a state the interface has to show rather than an
// error to report. See the Last.fm pane in PreferencesDialog.
//
// **Signing.** Every call carries an `api_sig`: the parameters sorted by name,
// concatenated as name-then-value with no separators, the shared secret
// appended, MD5 of the whole. `format` is excluded, and so is `api_sig` itself.
// That is Last.fm's specification and Cog's implementation agrees with it
// (LastFMAPI.swift:107-116); it is worth stating here because the failure mode
// of getting it wrong is error 13, "invalid method signature", which says
// nothing about which parameter was misplaced.

#pragma once

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/ScrobbleClient.hpp"

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {

/// A granted session. `key` does not expire; the listener revokes it from their
/// Last.fm account page rather than here.
struct LastFmSession {
    std::string key;
    std::string username;
};

class LastFmClient final : public IScrobbleClient {
public:
    /// `http` is borrowed and must outlive the client.
    ///
    /// The key and secret identify the *application* to Last.fm, not the
    /// listener. They are usually baked in at build time and may be empty -- see
    /// `configured()` -- and may be replaced later by `setCredentials()`.
    LastFmClient(IHttpClient& http, std::string apiKey, std::string apiSecret);

    /// Replaces the key and secret, for a listener who applied for their own
    /// rather than building with one.
    ///
    /// Safe to call while the scrobbler's worker is mid-request: each call reads
    /// both under a lock at the moment it signs, so a request is signed with one
    /// pair or the other, never a key from one and a secret from the other.
    /// Callers should expect a session key granted under the old pair to stop
    /// working -- a session belongs to the key that opened it.
    void setCredentials(std::string apiKey, std::string apiSecret);

    /// The key in use. For the interface to say which one that is; the secret
    /// is deliberately not offered back.
    [[nodiscard]] std::string apiKey() const;

    /// False when no API key is set, in which case every call here fails
    /// without touching the network. Cog ships in exactly this state:
    /// `Secrets.template.xcconfig` has both values blank and `AudioScrobbler`
    /// reports itself disabled.
    [[nodiscard]] bool configured() const override;

    // --- the desktop authentication flow --------------------------------

    /// Step 1. A request token, valid for 60 minutes and useless until granted.
    [[nodiscard]] std::optional<std::string> requestToken(ScrobbleError* error = nullptr);

    /// Step 2, which happens in a browser rather than here. Open this and let
    /// the listener grant access; there is nothing to send.
    [[nodiscard]] std::string authorizationUrl(std::string_view token) const;

    /// Step 3. Exchanges a granted token for a session key.
    ///
    /// Fails with `Kind::NotAuthorized` when the listener has not finished step
    /// 2 yet, which is not an error so much as "not yet" -- the caller polls or
    /// waits for a button.
    [[nodiscard]] std::optional<LastFmSession> session(std::string_view token,
                                                       ScrobbleError*   error = nullptr);

    // --- scrobbling -----------------------------------------------------

    bool updateNowPlaying(const ScrobbleTrack& track, std::string_view sessionKey,
                          ScrobbleError* error = nullptr) override;

    /// Submits up to `kMaxBatch` plays in one call. Last.fm answers 200 for a
    /// batch it partly rejected, so "accepted" and "ignored" are both read out
    /// of the body; the ignored reasons are things like "artist name was
    /// ignored" and "timestamp too far in the past", which are worth seeing
    /// when a scrobble does not appear.
    ///
    /// Batched because the queue is: a client that has been offline for an
    /// afternoon has a backlog, and fifty single submissions is fifty round
    /// trips against a rate limit. Last.fm's own limit for `track.scrobble` is
    /// 50 per request.
    [[nodiscard]] std::optional<ScrobbleResult> scrobble(
        std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
        ScrobbleError* error = nullptr) override;

    /// Last.fm's documented maximum for one `track.scrobble` call.
    static constexpr std::size_t kMaxBatch = 50;
    [[nodiscard]] std::size_t    maxBatch() const noexcept override { return kMaxBatch; }

    /// The signature over `params`, exposed because it is the one piece here
    /// worth pinning directly in a test: everything else can be checked through
    /// a fake transport, but a signature is only ever right or wrong and the
    /// server's complaint about a wrong one does not say why.
    ///
    /// `params` must not already contain `api_sig`. `format` is skipped.
    [[nodiscard]] static std::string signature(const HttpParams& params,
                                               std::string_view  secret);

private:
    /// Adds `api_key`, `format`, the signature, and `sk` when one is given, then
    /// sends. Every call above goes through here so none of them can forget one.
    [[nodiscard]] HttpResponse call(std::string_view method, HttpParams params,
                                    std::string_view sessionKey, bool usePost);

    /// Both under `credentialsMutex_`: written by the interface's thread and
    /// read by whichever worker is signing a request.
    [[nodiscard]] std::pair<std::string, std::string> credentials() const;

    IHttpClient&       http_;
    mutable std::mutex credentialsMutex_;
    std::string        apiKey_;
    std::string        apiSecret_;
};

}  // namespace xpcog
