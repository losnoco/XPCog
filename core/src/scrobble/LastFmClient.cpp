#include "xpcog/core/scrobble/LastFmClient.hpp"

#include "xpcog/core/Md5.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace xpcog {
namespace {

constexpr std::string_view kApiRoot  = "https://ws.audioscrobbler.com/2.0/";
constexpr std::string_view kAuthRoot = "https://www.last.fm/api/auth/";

/// Last.fm's error codes, only the ones whose handling differs.
/// https://www.last.fm/api/errorcodes
enum ApiCode : int {
    kInvalidService     = 2,
    kAuthenticationFailed = 4,
    kInvalidFormat      = 5,
    kInvalidParameters  = 6,
    kOperationFailed    = 8,
    kInvalidSessionKey  = 9,
    kInvalidApiKey      = 10,
    kServiceOffline     = 11,
    kInvalidSignature   = 13,
    kTokenNotAuthorized = 14,
    kTokenExpired       = 15,
    kTemporaryError     = 16,
    kSuspendedApiKey    = 26,
    kRateLimitExceeded  = 29,
};

void setError(LastFmError* error, LastFmError::Kind kind, int code, std::string message) {
    if (error != nullptr) {
        error->kind    = kind;
        error->code    = code;
        error->message = std::move(message);
    }
}

void clearError(LastFmError* error) {
    if (error != nullptr) {
        *error = LastFmError{};
    }
}

/// Turns a completed request into either a parsed body or an error.
///
/// Every failure mode a caller can meet is decided here rather than at four
/// call sites: a transport failure, a body that will not parse, Last.fm's own
/// error object, and the two codes that get their own Kind.
[[nodiscard]] std::optional<nlohmann::json> readReply(const HttpResponse& response,
                                                      LastFmError*        error) {
    if (response.transportFailed()) {
        setError(error, LastFmError::Kind::Transport, 0, response.error);
        return std::nullopt;
    }

    // parse() with allow_exceptions=false: a malformed body is an expected
    // outcome from a network, and this tree does not use exceptions for it.
    nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        // A non-JSON body with an HTTP error is the more useful message of the
        // two -- a 502 from an intermediary is not a Last.fm reply at all.
        if (response.status != 200) {
            setError(error, LastFmError::Kind::Transport, 0,
                     "HTTP " + std::to_string(response.status));
        } else {
            setError(error, LastFmError::Kind::Malformed, 0,
                     "could not parse the reply");
        }
        return std::nullopt;
    }

    if (body.contains("error")) {
        const int code = body["error"].is_number_integer()
                             ? body["error"].get<int>()
                             : 0;
        std::string message = body.value("message", std::string{});

        switch (code) {
        case kTokenNotAuthorized:
            setError(error, LastFmError::Kind::NotAuthorized, code, std::move(message));
            break;
        case kInvalidSessionKey:
            setError(error, LastFmError::Kind::SessionInvalid, code, std::move(message));
            break;
        default:
            setError(error, LastFmError::Kind::Api, code, std::move(message));
            break;
        }
        return std::nullopt;
    }

    if (response.status != 200) {
        setError(error, LastFmError::Kind::Transport, 0,
                 "HTTP " + std::to_string(response.status));
        return std::nullopt;
    }

    clearError(error);
    return body;
}

/// Appends a track's fields, suffixing each name when `index` is given.
///
/// The suffix is what makes a batch: `track.scrobble` takes `artist[0]`,
/// `artist[1]` and so on, while a single submission takes bare names. Written
/// once so the two forms cannot drift.
void appendTrack(HttpParams& params, const ScrobbleTrack& track,
                 std::optional<std::size_t> index) {
    const auto name = [&](std::string_view base) {
        std::string key{base};
        if (index) {
            key += '[';
            key += std::to_string(*index);
            key += ']';
        }
        return key;
    };

    params.emplace_back(name("artist"), track.artist);
    params.emplace_back(name("track"), track.title);

    if (track.startedAt > 0) {
        params.emplace_back(name("timestamp"), std::to_string(track.startedAt));
    }
    if (!track.album.empty()) {
        params.emplace_back(name("album"), track.album);
    }
    // Only when it differs from the track artist, which is Cog's rule
    // (AudioScrobbler.swift:83) and Last.fm's advice: sending them identical
    // makes every track on a single-artist album look like a compilation.
    if (!track.albumArtist.empty() && track.albumArtist != track.artist) {
        params.emplace_back(name("albumArtist"), track.albumArtist);
    }
    if (track.trackNumber > 0) {
        params.emplace_back(name("trackNumber"), std::to_string(track.trackNumber));
    }
    if (track.duration > 0.0) {
        params.emplace_back(name("duration"),
                            std::to_string(static_cast<long long>(track.duration)));
    }
    if (!track.musicBrainzId.empty()) {
        params.emplace_back(name("mbid"), track.musicBrainzId);
    }
}

}  // namespace

bool LastFmError::retryable() const noexcept {
    switch (kind) {
    case Kind::None:
        return false;
    case Kind::Transport:
        // Never reached the server, so the request is still unmade.
        return true;
    case Kind::Malformed:
        // The server answered something unreadable. Rare, and more likely a
        // captive portal or a proxy than Last.fm, so worth trying again.
        return true;
    case Kind::NotAuthorized:
        // Retryable in the auth flow's sense -- the listener may yet grant it --
        // but this is never reached from the queue, which only ever holds
        // scrobbles.
        return true;
    case Kind::SessionInvalid:
        // Retrying cannot help; the listener has to authorise again.
        return false;
    case Kind::Api:
        break;
    }

    switch (code) {
    case kOperationFailed:
    case kServiceOffline:
    case kTemporaryError:
    case kRateLimitExceeded:
        return true;
    default:
        // Everything else is a statement about the request rather than about the
        // moment: a bad signature, a suspended key or a rejected parameter will
        // be just as bad in an hour. Dropping them is what keeps one poisoned
        // entry from blocking the queue behind it.
        return false;
    }
}

LastFmClient::LastFmClient(IHttpClient& http, std::string apiKey, std::string apiSecret)
    : http_(http), apiKey_(std::move(apiKey)), apiSecret_(std::move(apiSecret)) {}

bool LastFmClient::configured() const noexcept {
    return !apiKey_.empty() && !apiSecret_.empty();
}

std::string LastFmClient::signature(const HttpParams& params, std::string_view secret) {
    // Sorted by name. A copy rather than sorting in place: the caller's order is
    // the body's order, and only the signature wants this one.
    HttpParams sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string joined;
    for (const auto& [key, value] : sorted) {
        // `format` is excluded from the signature by Last.fm's specification,
        // and `api_sig` obviously cannot sign itself.
        if (key == "format" || key == "api_sig") {
            continue;
        }
        joined += key;
        joined += value;
    }
    joined += secret;

    // The *unencoded* values are signed; percent-encoding happens on the way
    // out. Signing the encoded form is the classic way to get error 13.
    return md5Hex(joined);
}

HttpResponse LastFmClient::call(std::string_view method, HttpParams params,
                                std::string_view sessionKey, bool usePost) {
    params.emplace_back("method", std::string{method});
    params.emplace_back("api_key", apiKey_);
    if (!sessionKey.empty()) {
        params.emplace_back("sk", std::string{sessionKey});
    }

    // Signed before `format` is added, which costs nothing since signature()
    // skips it, and keeps the two orderings from having to agree.
    params.emplace_back("api_sig", signature(params, apiSecret_));
    params.emplace_back("format", "json");

    return usePost ? http_.post(kApiRoot, params) : http_.get(kApiRoot, params);
}

std::optional<std::string> LastFmClient::requestToken(LastFmError* error) {
    if (!configured()) {
        setError(error, LastFmError::Kind::Api, kInvalidApiKey,
                 "this build carries no Last.fm API key");
        return std::nullopt;
    }

    const HttpResponse response = call("auth.getToken", {}, {}, /*usePost=*/false);

    const std::optional<nlohmann::json> body = readReply(response, error);
    if (!body) {
        return std::nullopt;
    }

    const auto token = body->value("token", std::string{});
    if (token.empty()) {
        setError(error, LastFmError::Kind::Malformed, 0, "the reply carried no token");
        return std::nullopt;
    }
    return token;
}

std::string LastFmClient::authorizationUrl(std::string_view token) const {
    std::string url{kAuthRoot};
    url += "?api_key=";
    url += percentEncode(apiKey_);
    url += "&token=";
    url += percentEncode(token);
    return url;
}

std::optional<LastFmSession> LastFmClient::session(std::string_view token,
                                                   LastFmError*     error) {
    if (!configured()) {
        setError(error, LastFmError::Kind::Api, kInvalidApiKey,
                 "this build carries no Last.fm API key");
        return std::nullopt;
    }

    HttpParams params;
    params.emplace_back("token", std::string{token});

    // GET, as Last.fm's own example for this call does.
    const HttpResponse response =
        call("auth.getSession", std::move(params), {}, /*usePost=*/false);

    const std::optional<nlohmann::json> body = readReply(response, error);
    if (!body) {
        return std::nullopt;
    }

    const auto it = body->find("session");
    if (it == body->end() || !it->is_object()) {
        setError(error, LastFmError::Kind::Malformed, 0, "the reply carried no session");
        return std::nullopt;
    }

    LastFmSession granted;
    granted.key      = it->value("key", std::string{});
    granted.username = it->value("name", std::string{});
    if (granted.key.empty()) {
        setError(error, LastFmError::Kind::Malformed, 0,
                 "the session carried no key");
        return std::nullopt;
    }
    return granted;
}

bool LastFmClient::updateNowPlaying(const ScrobbleTrack& track,
                                    std::string_view sessionKey, LastFmError* error) {
    if (!configured() || track.artist.empty() || track.title.empty()) {
        setError(error, LastFmError::Kind::Api, kInvalidParameters,
                 "a now-playing update needs an artist and a title");
        return false;
    }

    HttpParams params;
    // No timestamp: this call is about the present, and sending one is not part
    // of its parameter list.
    ScrobbleTrack present = track;
    present.startedAt     = 0;
    appendTrack(params, present, std::nullopt);

    const HttpResponse response =
        call("track.updateNowPlaying", std::move(params), sessionKey, /*usePost=*/true);
    return readReply(response, error).has_value();
}

std::optional<LastFmClient::ScrobbleResult> LastFmClient::scrobble(
    std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
    LastFmError* error) {
    if (!configured()) {
        setError(error, LastFmError::Kind::Api, kInvalidApiKey,
                 "this build carries no Last.fm API key");
        return std::nullopt;
    }
    if (tracks.empty() || tracks.size() > kMaxBatch) {
        setError(error, LastFmError::Kind::Api, kInvalidParameters,
                 "a batch holds between one and fifty scrobbles");
        return std::nullopt;
    }

    HttpParams params;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        appendTrack(params, tracks[i], i);
    }

    const HttpResponse response =
        call("track.scrobble", std::move(params), sessionKey, /*usePost=*/true);

    const std::optional<nlohmann::json> body = readReply(response, error);
    if (!body) {
        return std::nullopt;
    }

    ScrobbleResult result;

    // `scrobbles.@attr` carries the counts. Both are quoted numbers in Last.fm's
    // JSON, which is why they are read as strings first.
    const auto scrobbles = body->find("scrobbles");
    if (scrobbles == body->end() || !scrobbles->is_object()) {
        setError(error, LastFmError::Kind::Malformed, 0,
                 "the reply carried no scrobble result");
        return std::nullopt;
    }

    const auto readCount = [](const nlohmann::json& holder, const char* key) {
        const auto it = holder.find(key);
        if (it == holder.end()) {
            return 0;
        }
        if (it->is_number_integer()) {
            return it->get<int>();
        }
        if (it->is_string()) {
            const std::string text = it->get<std::string>();
            try {
                return std::stoi(text);
            } catch (...) {
                return 0;
            }
        }
        return 0;
    };

    if (const auto attr = scrobbles->find("@attr"); attr != scrobbles->end()) {
        result.accepted = readCount(*attr, "accepted");
        result.ignored  = readCount(*attr, "ignored");
    }

    // The per-entry `ignoredMessage` explains a rejection. One entry is an
    // object; several are an array -- Last.fm's JSON does not keep the shape
    // stable across batch sizes, which is worth handling rather than being
    // surprised by.
    const auto entry = scrobbles->find("scrobble");
    if (entry != scrobbles->end()) {
        const auto readIgnored = [&](const nlohmann::json& one) {
            if (!one.is_object()) {
                return;
            }
            const auto message = one.find("ignoredMessage");
            if (message == one.end() || !message->is_object()) {
                return;
            }
            const std::string text = message->value("#text", std::string{});
            if (!text.empty() && result.ignoredReason.empty()) {
                result.ignoredReason = text;
            }
        };

        if (entry->is_array()) {
            for (const auto& one : *entry) {
                readIgnored(one);
            }
        } else {
            readIgnored(*entry);
        }
    }

    return result;
}

}  // namespace xpcog
