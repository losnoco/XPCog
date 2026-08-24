// A small synchronous HTTP client, and the seam that keeps the network out of
// the tests.
//
// This is deliberately not the transport `codecs/httpsource` uses. That one
// streams: a worker thread owns a transfer that never ends, fills a ring, and is
// restarted with a Range header on a seek. This one makes a request, waits for
// the whole answer, and returns it -- which is all an API call is, and which the
// streaming shape cannot express without pretending a 300-byte JSON reply is a
// radio station.
//
// **Synchronous on purpose.** Every caller here already has a thread it would
// rather block: the scrobbler owns a submission worker precisely so a request
// that takes four seconds does not take the interface with it. An asynchronous
// client would put a second concurrency model underneath one that already
// works, and callbacks arriving on a curl thread are exactly the marshalling
// problem `platform/` had to solve for SMTC.
//
// The interface exists so that `LastFmClient` can be driven by a fake in tests.
// That is not a hypothetical benefit: the auth flow is four round trips with a
// signature over each, and pinning those against a real server would mean a test
// that needs credentials, a network, and somebody's actual listening history.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog {

/// One request's outcome.
///
/// A transport failure and an HTTP error are different things and are reported
/// differently: `error` non-empty means the request never completed, and
/// `status` is meaningless. A 403 with a body is a completed request that the
/// server refused, which for Last.fm carries a machine-readable reason the
/// caller wants to read rather than discard.
struct HttpResponse {
    long        status = 0;
    std::string body;

    /// Empty when the transport succeeded, whatever the status was.
    std::string error;

    [[nodiscard]] bool transportFailed() const noexcept { return !error.empty(); }
    [[nodiscard]] bool ok() const noexcept { return error.empty() && status == 200; }
};

/// Form parameters, in the order the caller built them. A vector rather than a
/// map because Last.fm's signature is over the *sorted* set and the body is not,
/// so a container that imposed one order would have to be re-sorted for the
/// other anyway -- and because a batched scrobble legitimately repeats a key
/// under different indices (`artist[0]`, `artist[1]`).
using HttpParams = std::vector<std::pair<std::string, std::string>>;

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    /// `params` are form-encoded into the body as
    /// application/x-www-form-urlencoded.
    [[nodiscard]] virtual HttpResponse post(std::string_view  url,
                                            const HttpParams& params) = 0;

    /// `params` are percent-encoded into the query string.
    [[nodiscard]] virtual HttpResponse get(std::string_view  url,
                                           const HttpParams& params) = 0;
};

/// The libcurl-backed client, or **null** in a build configured without
/// `XPCOG_WITH_HTTP`.
///
/// Null rather than a throwing stub, because the one caller has to degrade
/// gracefully anyway: a player built without HTTP has no internet radio either,
/// and the preferences pane greys the row and says so -- the same shape as the
/// crash-reporting checkbox in a build without Sentry.
[[nodiscard]] std::unique_ptr<IHttpClient> makeCurlHttpClient();

/// True when this build has a real HTTP client to hand out. Lets a caller ask
/// before constructing anything, which is what the preferences pane wants.
[[nodiscard]] bool httpClientAvailable() noexcept;

/// Percent-encodes for a query string or a form body: everything but
/// `A-Za-z0-9-_.~` becomes %XX, and a space is %20 rather than '+'.
///
/// Exposed because it is tested directly. Last.fm signs the *unencoded* values
/// and sends the encoded ones, so an encoder that disagreed with the server's
/// decoder would produce a valid signature over a body that arrived as
/// something else -- a failure that reads as "invalid signature" and sends you
/// looking at the wrong half.
[[nodiscard]] std::string percentEncode(std::string_view text);

/// Joins params into `a=1&b=2`, percent-encoding both halves.
[[nodiscard]] std::string formEncode(const HttpParams& params);

}  // namespace xpcog
