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

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog {

/// One play, as Last.fm wants to be told about it.
///
/// `artist` and `title` are the only required fields, which is Last.fm's rule
/// rather than a simplification: a submission missing either is rejected, so
/// Scrobbler refuses to queue one rather than sending it to be refused.
struct ScrobbleTrack {
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string musicBrainzId;

    /// 0 when unknown, and omitted from the request in that case rather than
    /// sent as zero -- Cog does the same (AudioScrobbler.swift:88).
    int trackNumber = 0;

    /// Seconds. 0 when unknown; omitted rather than sent as zero.
    double duration = 0.0;

    /// UTC seconds since the Unix epoch, at the moment the track **started**
    /// playing -- not when the threshold was reached and not when it was
    /// submitted. Last.fm builds the listening history from this, so a queued
    /// scrobble sent an hour late still lands in the right place.
    ///
    /// Unused by `updateNowPlaying`, which is about the present by definition.
    std::int64_t startedAt = 0;
};

/// A granted session. `key` does not expire; the listener revokes it from their
/// Last.fm account page rather than here.
struct LastFmSession {
    std::string key;
    std::string username;
};

/// Why a call did not succeed.
struct LastFmError {
    enum class Kind : std::uint8_t {
        None,
        /// The request never reached the server. Always worth retrying.
        Transport,
        /// The server answered with one of its own error codes.
        Api,
        /// The server answered with something this could not read.
        Malformed,
        /// Step 2 of the auth flow has not happened yet: the listener has not
        /// visited the authorisation page. Its own kind rather than an API
        /// error, because it is the expected state while a connection is in
        /// progress and the interface polls through it.
        NotAuthorized,
        /// The stored session key is no longer valid and the listener has to
        /// authorise again. Its own kind because it is the one failure whose
        /// correct handling is to *discard credentials*, which no other error
        /// justifies.
        SessionInvalid,
    };

    Kind        kind = Kind::None;
    int         code = 0;  ///< Last.fm's code when `kind == Kind::Api`.
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return kind == Kind::None; }

    /// Whether sending the same request again later could succeed.
    ///
    /// The distinction is what makes an offline queue safe: a retryable failure
    /// keeps the scrobble, and a permanent one drops it. Retrying a rejected
    /// submission forever would mean one bad entry blocking every later one.
    [[nodiscard]] bool retryable() const noexcept;
};

class LastFmClient {
public:
    /// `http` is borrowed and must outlive the client.
    ///
    /// The key and secret are the *application's*, not the listener's. They are
    /// baked in at build time and may be empty -- see `configured()`.
    LastFmClient(IHttpClient& http, std::string apiKey, std::string apiSecret);

    /// False when this build carries no API key, in which case every call here
    /// fails without touching the network. Cog ships in exactly this state:
    /// `Secrets.template.xcconfig` has both values blank and `AudioScrobbler`
    /// reports itself disabled.
    [[nodiscard]] bool configured() const noexcept;

    // --- the desktop authentication flow --------------------------------

    /// Step 1. A request token, valid for 60 minutes and useless until granted.
    [[nodiscard]] std::optional<std::string> requestToken(LastFmError* error = nullptr);

    /// Step 2, which happens in a browser rather than here. Open this and let
    /// the listener grant access; there is nothing to send.
    [[nodiscard]] std::string authorizationUrl(std::string_view token) const;

    /// Step 3. Exchanges a granted token for a session key.
    ///
    /// Fails with `Kind::NotAuthorized` when the listener has not finished step
    /// 2 yet, which is not an error so much as "not yet" -- the caller polls or
    /// waits for a button.
    [[nodiscard]] std::optional<LastFmSession> session(std::string_view token,
                                                       LastFmError*     error = nullptr);

    // --- scrobbling -----------------------------------------------------

    /// "This is playing now." Fire-and-forget by design: Last.fm keeps it for a
    /// few minutes and it is never part of the listening history, so a failure
    /// here is not worth queueing or retrying.
    bool updateNowPlaying(const ScrobbleTrack& track, std::string_view sessionKey,
                          LastFmError* error = nullptr);

    /// What a batch submission did. Last.fm answers 200 for a batch it partly
    /// rejected, so "accepted" and "ignored" both have to be read out of the
    /// body -- a submission that silently vanished is otherwise indistinguishable
    /// from one that worked.
    struct ScrobbleResult {
        int accepted = 0;
        int ignored  = 0;
        /// The reason the server gave for the first ignored entry, when it gave
        /// one. Kept for the log rather than for the listener: the codes are
        /// things like "artist name was ignored" and "timestamp too far in the
        /// past", which are worth seeing when a scrobble does not appear.
        std::string ignoredReason;
    };

    /// Submits up to `kMaxBatch` plays in one call.
    ///
    /// Batched because the queue is: a client that has been offline for an
    /// afternoon has a backlog, and fifty single submissions is fifty round
    /// trips against a rate limit. Last.fm's own limit for `track.scrobble` is
    /// 50 per request.
    [[nodiscard]] std::optional<ScrobbleResult> scrobble(
        std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
        LastFmError* error = nullptr);

    /// Last.fm's documented maximum for one `track.scrobble` call.
    static constexpr std::size_t kMaxBatch = 50;

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

    IHttpClient& http_;
    std::string  apiKey_;
    std::string  apiSecret_;
};

}  // namespace xpcog
