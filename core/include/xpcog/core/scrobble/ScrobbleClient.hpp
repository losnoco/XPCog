// What a scrobbling service looks like from the queue's side.
//
// The Scrobbler was written against Last.fm and it turned out to need very
// little of it: "is this client usable at all", "announce this track", "submit
// these plays under this session", and a verdict on what went wrong that says
// whether waiting could help. Everything else -- the signature, the three-step
// grant, the error-code table -- is the service's own business. ListenBrainz
// needs the same four things and none of the rest, so the four are the seam.
//
// The session key is an opaque string here on purpose. For Last.fm it is the
// key `auth.getSession` granted; for ListenBrainz it is the user token off the
// settings page. The queue does not care which, and it must not, because the
// queue is the part that has to be right for both.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xpcog {

/// One play, as a scrobbling service wants to be told about it.
///
/// `artist` and `title` are the only required fields, which is both services'
/// rule rather than a simplification: a submission missing either is rejected,
/// so Scrobbler refuses to queue one rather than sending it to be refused.
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
    /// submitted. Both services build the listening history from this, so a
    /// queued scrobble sent an hour late still lands in the right place.
    ///
    /// Unused by `updateNowPlaying`, which is about the present by definition.
    std::int64_t startedAt = 0;
};

/// Why a call did not succeed.
struct ScrobbleError {
    enum class Kind : std::uint8_t {
        None,
        /// The request never reached the server. Always worth retrying.
        Transport,
        /// The server refused the request itself: a bad parameter, a bad
        /// signature, a suspended key. It will be just as bad in an hour.
        Api,
        /// The server said the *moment* was wrong rather than the request --
        /// offline, overloaded, rate-limited. Worth retrying later. Its own
        /// kind rather than a flag on `Api` so that the queue can ask
        /// `retryable()` without knowing whose codes it is looking at.
        Transient,
        /// The server answered with something this could not read.
        Malformed,
        /// Step 2 of Last.fm's auth flow has not happened yet: the listener has
        /// not visited the authorisation page. Its own kind rather than an API
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
    /// The service's own code: Last.fm's error number, ListenBrainz's HTTP
    /// status. For the log; nothing dispatches on it above the client.
    int         code = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return kind == Kind::None; }

    /// Whether sending the same request again later could succeed.
    ///
    /// The distinction is what makes an offline queue safe: a retryable failure
    /// keeps the scrobble, and a permanent one drops it. Retrying a rejected
    /// submission forever would mean one bad entry blocking every later one.
    [[nodiscard]] bool retryable() const noexcept;
};

/// What a batch submission did. Last.fm answers 200 for a batch it partly
/// rejected, so "accepted" and "ignored" both have to be read out of the body
/// -- a submission that silently vanished is otherwise indistinguishable from
/// one that worked. ListenBrainz accepts or refuses a batch whole.
struct ScrobbleResult {
    int accepted = 0;
    int ignored  = 0;
    /// The reason the server gave for the first ignored entry, when it gave
    /// one. Kept for the log rather than for the listener.
    std::string ignoredReason;
};

class IScrobbleClient {
public:
    virtual ~IScrobbleClient() = default;

    /// False when the client cannot make any request -- a Last.fm client with
    /// no API key -- in which case every call fails without touching the
    /// network.
    [[nodiscard]] virtual bool configured() const = 0;

    /// The most plays one `scrobble()` call may carry.
    [[nodiscard]] virtual std::size_t maxBatch() const noexcept = 0;

    /// "This is playing now." Fire-and-forget by design: both services keep it
    /// for a few minutes and it is never part of the listening history, so a
    /// failure here is not worth queueing or retrying.
    virtual bool updateNowPlaying(const ScrobbleTrack& track, std::string_view sessionKey,
                                  ScrobbleError* error = nullptr) = 0;

    /// Submits up to `maxBatch()` plays in one call. `tracks` must not be
    /// empty.
    [[nodiscard]] virtual std::optional<ScrobbleResult> scrobble(
        std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
        ScrobbleError* error = nullptr) = 0;
};

}  // namespace xpcog
