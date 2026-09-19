#include "xpcog/core/scrobble/ListenBrainzClient.hpp"

#include "xpcog/core/Version.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace xpcog {
namespace {

void setError(ScrobbleError* error, ScrobbleError::Kind kind, int code,
              std::string message) {
    if (error != nullptr) {
        error->kind    = kind;
        error->code    = code;
        error->message = std::move(message);
    }
}

void clearError(ScrobbleError* error) {
    if (error != nullptr) {
        *error = ScrobbleError{};
    }
}

/// The server's own explanation, when its body carries one, or the status.
[[nodiscard]] std::string messageOf(const HttpResponse& response) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_object()) {
        if (const auto it = body.find("error"); it != body.end() && it->is_string()) {
            return it->get<std::string>();
        }
        if (const auto it = body.find("message"); it != body.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return "HTTP " + std::to_string(response.status);
}

/// Turns a status into the queue's vocabulary. ListenBrainz speaks in HTTP:
/// 401 is a token it does not know, 429 is the rate limit, 5xx is its own
/// trouble, and 400 is a request it will never take.
[[nodiscard]] bool verdict(const HttpResponse& response, ScrobbleError* error) {
    if (response.transportFailed()) {
        setError(error, ScrobbleError::Kind::Transport, 0, response.error);
        return false;
    }
    if (response.status == 200) {
        clearError(error);
        return true;
    }
    const int status = static_cast<int>(response.status);
    if (status == 401) {
        setError(error, ScrobbleError::Kind::SessionInvalid, status, messageOf(response));
    } else if (status == 429 || status == 503 || status == 502 || status == 504 ||
               status == 500) {
        setError(error, ScrobbleError::Kind::Transient, status, messageOf(response));
    } else {
        setError(error, ScrobbleError::Kind::Api, status, messageOf(response));
    }
    return false;
}

[[nodiscard]] HttpHeaders authorization(std::string_view token) {
    return {{"Authorization", "Token " + std::string{token}}};
}

/// One listen, in the shape the server documents. `listened_at` is left out
/// for a now-playing announcement, which the server requires.
[[nodiscard]] nlohmann::json listenOf(const ScrobbleTrack& track, bool withTimestamp) {
    nlohmann::json metadata;
    metadata["artist_name"] = track.artist;
    metadata["track_name"]  = track.title;
    if (!track.album.empty()) {
        metadata["release_name"] = track.album;
    }

    // Who is submitting, which the server asks for and which is what tells a
    // listener's history apart from the same plays sent by another client.
    nlohmann::json info;
    info["media_player"]              = "XPCog";
    info["media_player_version"]      = std::string{kVersionString};
    info["submission_client"]         = "XPCog";
    info["submission_client_version"] = std::string{kVersionString};
    if (track.trackNumber > 0) {
        // A string, as the server documents it.
        info["tracknumber"] = std::to_string(track.trackNumber);
    }
    if (track.duration > 0.0) {
        info["duration_ms"] = static_cast<long long>(track.duration * 1000.0);
    }
    if (!track.musicBrainzId.empty()) {
        info["recording_mbid"] = track.musicBrainzId;
    }
    metadata["additional_info"] = std::move(info);

    nlohmann::json listen;
    if (withTimestamp) {
        listen["listened_at"] = track.startedAt;
    }
    listen["track_metadata"] = std::move(metadata);
    return listen;
}

}  // namespace

ListenBrainzClient::ListenBrainzClient(IHttpClient& http, std::string apiRoot)
    : http_(http), apiRoot_(std::move(apiRoot)) {}

void ListenBrainzClient::setApiRoot(std::string apiRoot) {
    std::lock_guard lock(rootMutex_);
    apiRoot_ = std::move(apiRoot);
}

std::string ListenBrainzClient::apiRoot() const {
    std::lock_guard lock(rootMutex_);
    return apiRoot_;
}

std::string ListenBrainzClient::endpoint(std::string_view name) const {
    std::string root = apiRoot();
    if (root.empty()) {
        root = kDefaultApiRoot;
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    // Both "https://host" and "https://host/1" are written down as the root by
    // people who have read different pages; the version is added only when it
    // is not already there.
    if (!root.ends_with("/1")) {
        root += "/1";
    }
    root += '/';
    root += name;
    return root;
}

std::optional<std::string> ListenBrainzClient::validateToken(std::string_view token,
                                                             ScrobbleError* error) {
    if (token.empty()) {
        setError(error, ScrobbleError::Kind::SessionInvalid, 0, "no token");
        return std::nullopt;
    }
    const HttpResponse response =
        http_.get(endpoint("validate-token"), {}, authorization(token));
    if (!verdict(response, error)) {
        return std::nullopt;
    }

    // 200 for a valid token *and* for an invalid one; the body says which.
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.contains("valid")) {
        setError(error, ScrobbleError::Kind::Malformed, 0, "could not parse the reply");
        return std::nullopt;
    }
    if (!body["valid"].is_boolean() || !body["valid"].get<bool>()) {
        setError(error, ScrobbleError::Kind::SessionInvalid, 200,
                 body.value("message", std::string{"Token invalid."}));
        return std::nullopt;
    }
    clearError(error);
    return body.value("user_name", std::string{});
}

bool ListenBrainzClient::submit(std::string_view body, std::string_view token,
                                ScrobbleError* error) {
    if (token.empty()) {
        setError(error, ScrobbleError::Kind::SessionInvalid, 0, "no token");
        return false;
    }
    const HttpResponse response =
        http_.postJson(endpoint("submit-listens"), body, authorization(token));
    return verdict(response, error);
}

bool ListenBrainzClient::updateNowPlaying(const ScrobbleTrack& track,
                                          std::string_view sessionKey, ScrobbleError* error) {
    nlohmann::json request;
    request["listen_type"] = "playing_now";
    request["payload"]     = nlohmann::json::array({listenOf(track, false)});
    return submit(request.dump(), sessionKey, error);
}

std::optional<ScrobbleResult> ListenBrainzClient::scrobble(
    std::span<const ScrobbleTrack> tracks, std::string_view sessionKey,
    ScrobbleError* error) {
    if (tracks.empty() || tracks.size() > kMaxBatch) {
        setError(error, ScrobbleError::Kind::Api, 0, "batch size out of range");
        return std::nullopt;
    }

    nlohmann::json payload = nlohmann::json::array();
    for (const ScrobbleTrack& track : tracks) {
        payload.push_back(listenOf(track, true));
    }
    const std::string_view listenType = (tracks.size() == 1) ? "single" : "import";

    nlohmann::json request;
    request["listen_type"] = listenType;
    request["payload"]     = std::move(payload);
    if (!submit(request.dump(), sessionKey, error)) {
        return std::nullopt;
    }

    // The server accepts a batch whole or refuses it whole; there is no
    // partial verdict to read out.
    ScrobbleResult result;
    result.accepted = static_cast<int>(tracks.size());
    return result;
}

}  // namespace xpcog
