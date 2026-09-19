// The ListenBrainz API, as much of it as scrobbling needs.
//
// Not a port of anything: Cog scrobbles to Last.fm and nowhere else. It is here
// because the queue that makes scrobbling trustworthy -- Scrobbler.hpp -- was
// the hard part, and ListenBrainz was a second service for the price of a
// client that speaks its two calls. It is also the service a listener who
// would rather not feed a commercial one reaches for, and it takes the same
// plays: artist, title, when it started.
//
// Three things make it simpler than Last.fm, and they shape this file:
//
//   * **No application key.** Last.fm wants to know which program is
//     submitting and signs every request with that program's secret;
//     ListenBrainz identifies the *listener* only, through a user token off
//     their settings page, sent as `Authorization: Token ...`. So there is
//     nothing to sign, nothing to bake in at build time, and `configured()` is
//     always true.
//   * **No grant flow.** The listener copies the token from
//     listenbrainz.org/settings and pastes it in; `validateToken()` asks the
//     server whose it is, which is the whole of connecting.
//   * **JSON, not form fields.** One body shape for a single play, a
//     now-playing announcement and a backlog, distinguished by `listen_type`.
//
// One thing makes it a little more general: the API is spoken by more than the
// one server. A self-hosted ListenBrainz, and Maloja with its compatibility
// endpoint, take the same requests at a different root, so the root is a
// parameter and the pane has a field for it.
//
// https://listenbrainz.readthedocs.io/en/latest/users/api/core.html

#pragma once

#include "xpcog/core/net/HttpClient.hpp"
#include "xpcog/core/scrobble/ScrobbleClient.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xpcog {

class ListenBrainzClient final : public IScrobbleClient {
public:
    /// The public service's API root, and what `apiRoot()` answers until told
    /// otherwise.
    static constexpr std::string_view kDefaultApiRoot = "https://api.listenbrainz.org";

    /// `http` is borrowed and must outlive the client. `apiRoot` is the
    /// server's base, with or without a trailing slash and with or without the
    /// `/1` the API lives under -- both spellings are common in the wild and
    /// both are accepted.
    ListenBrainzClient(IHttpClient& http, std::string apiRoot = std::string{kDefaultApiRoot});

    /// Points the client at another server. Safe to call while the scrobbler's
    /// worker is mid-request; each request reads the root once, under a lock.
    void setApiRoot(std::string apiRoot);
    [[nodiscard]] std::string apiRoot() const;

    /// Always true: there is no application key to be missing.
    [[nodiscard]] bool configured() const override { return true; }

    /// Asks the server who `token` belongs to. The username on success; on
    /// failure, `Kind::SessionInvalid` for a token the server does not know,
    /// which is the answer "connect" gets for a mistyped paste.
    [[nodiscard]] std::optional<std::string> validateToken(std::string_view token,
                                                           ScrobbleError* error = nullptr);

    bool updateNowPlaying(const ScrobbleTrack& track, std::string_view sessionKey,
                          ScrobbleError* error = nullptr) override;

    /// One play goes as `single` and several as `import`, which are the same
    /// request with a different label: the server documents `single` for a
    /// track just heard and `import` for a backlog, and a queue that has been
    /// offline is exactly a backlog.
    [[nodiscard]] std::optional<ScrobbleResult> scrobble(
        std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
        ScrobbleError* error = nullptr) override;

    /// The server takes a thousand per request. Fifty is Last.fm's number and
    /// it is kept here on purpose: a batch is dropped whole when the server
    /// refuses it, and a thousand plays lost to one malformed entry is a
    /// different order of harm from fifty.
    static constexpr std::size_t kMaxBatch = 50;
    [[nodiscard]] std::size_t    maxBatch() const noexcept override { return kMaxBatch; }

private:
    /// `/1/<endpoint>` under the root in use, whichever spelling the root had.
    [[nodiscard]] std::string endpoint(std::string_view name) const;

    /// Posts `body` under `token` and reads the verdict. Every submission goes
    /// through here so the two cannot disagree about what a status means.
    bool submit(std::string_view body, std::string_view token, ScrobbleError* error);

    IHttpClient&       http_;
    mutable std::mutex rootMutex_;
    std::string        apiRoot_;
};

}  // namespace xpcog
