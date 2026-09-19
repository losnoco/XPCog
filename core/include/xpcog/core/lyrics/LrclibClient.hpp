// The LRCLIB API, as much of it as looking up one track's lyrics needs.
//
// Not a port of anything: Cog shows the lyrics a file carries and nothing
// else. It is here because the pane that shows them already existed
// (app/src/LyricsPanel.hpp) and was blank for most of a collection, and
// because LRCLIB is the one lyrics service that asks for nothing -- no key, no
// account, no rate-limit token -- and answers a machine-friendly JSON to a
// single GET. https://lrclib.net/docs is the reference; the server is open
// source (github.com/tranxuanthang/lrclib), which is where the details below
// that the page does not spell out were read.
//
// One call: `GET /api/get?track_name=&artist_name=&album_name=&duration=`.
// The server lowercases and strips punctuation from the names before matching
// (server/src/utils.rs, `prepare_input`), so case and a stray apostrophe do
// not matter, but the words must be the ones the track was published under --
// this is a lookup, not a search. `duration` is matched to within two seconds
// (track_repository.rs, `get_track_by_metadata`) and must be between 1 and
// 3600 or the request is refused outright as a validation error, so an unknown
// or absurd length is left out rather than sent. A track the service knows
// answers 200 with `plainLyrics`, `syncedLyrics` and `instrumental`; one it
// does not answers 404 with `{"name":"TrackNotFound"}`, which is an answer
// rather than a failure and is reported as such. The service also has
// `/api/search`, which takes a free-text query and returns candidates;
// deliberately not used yet, because a guessed match shown as the song's
// lyrics is worse than an honest blank.
//
// **Synced lyrics are carried and not yet shown.** The reply has both, and the
// panel does not yet have a way to follow playback through an LRC file, so
// `synced` rides along in the struct for whoever builds that.
//
// The server reads a `Lrclib-Client` header to know which program is asking,
// and its documentation asks clients to send one naming the program, its
// version and where to find it. Every request here does.
//
// Like ListenBrainz, the API is spoken by more than the one server -- the
// README documents running your own -- so the root is a parameter.

#pragma once

#include "xpcog/core/net/HttpClient.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog {

/// What identifies a track to the service. `title` and `artist` are required;
/// a query missing either is refused here rather than sent, because the server
/// would refuse it too and say less about why.
struct LyricsQuery {
    std::string title;
    std::string artist;
    /// Narrows the match when given; the server matches without it otherwise.
    std::string album;
    /// Seconds. Zero when unknown, which leaves it out of the request.
    double duration = 0.0;
};

/// What the service knows about a track it has.
struct LrclibLyrics {
    /// The words, one line per line, or empty when the entry has none. An
    /// instrumental has none by definition; a track that has only a synced
    /// file has a plain version derived from it by the server.
    std::string plain;
    /// The same words with `[mm:ss.xx]` timestamps, LRC format, or empty.
    std::string synced;
    /// Published as having no words. Distinct from "not found": the service is
    /// saying there is nothing to show, not that it has not looked.
    bool instrumental = false;
};

/// Why a lookup produced nothing.
struct LyricsError {
    enum class Kind {
        None,
        /// The service does not have this track. An answer, not a fault.
        NotFound,
        /// The request never completed: no network, no DNS, timed out.
        Transport,
        /// The server was busy or briefly broken (429, 5xx). Asking again
        /// later may well succeed.
        Transient,
        /// The server refused the request as such (400 and the rest).
        Api,
        /// A 200 whose body was not the JSON the API documents.
        Malformed,
        /// The query had no title or no artist, so nothing was sent.
        Incomplete,
    };
    Kind        kind = Kind::None;
    int         code = 0;  ///< HTTP status when the server answered, else 0.
    std::string message;
};

class LrclibClient {
public:
    /// The public service, and what `apiRoot()` answers until told otherwise.
    static constexpr std::string_view kDefaultApiRoot = "https://lrclib.net";

    /// `http` is borrowed and must outlive the client. `apiRoot` is the
    /// server's base, with or without a trailing slash and with or without
    /// the `/api` the routes live under.
    explicit LrclibClient(IHttpClient& http,
                          std::string  apiRoot = std::string{kDefaultApiRoot});

    /// Points the client at another server. Safe to call while a lookup is
    /// mid-request on another thread; each request reads the root once.
    void setApiRoot(std::string apiRoot);
    [[nodiscard]] std::string apiRoot() const;

    /// Asks the service for `query`'s lyrics. The entry on success -- which
    /// may be an instrumental, or carry only a synced file -- and nullopt with
    /// `error` filled in otherwise, `Kind::NotFound` included.
    [[nodiscard]] std::optional<LrclibLyrics> get(const LyricsQuery& query,
                                                  LyricsError*       error = nullptr);

    /// The `Lrclib-Client` value every request carries: `userAgent()` from
    /// Version.hpp -- the program, its version, and where to find it, as the
    /// service asks -- under the header name the server reads first. Exposed
    /// for the test that pins it.
    [[nodiscard]] static std::string clientHeader();

private:
    /// `/api/<name>` under the root in use, whichever spelling the root had.
    [[nodiscard]] std::string endpoint(std::string_view name) const;

    IHttpClient&       http_;
    mutable std::mutex rootMutex_;
    std::string        apiRoot_;
};

}  // namespace xpcog
