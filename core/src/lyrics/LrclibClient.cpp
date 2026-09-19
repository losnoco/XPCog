#include "xpcog/core/lyrics/LrclibClient.hpp"

#include "xpcog/core/Version.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <utility>

namespace xpcog {
namespace {

void setError(LyricsError* error, LyricsError::Kind kind, int code, std::string message) {
    if (error != nullptr) {
        error->kind    = kind;
        error->code    = code;
        error->message = std::move(message);
    }
}

void clearError(LyricsError* error) {
    if (error != nullptr) {
        *error = LyricsError{};
    }
}

/// The server's own explanation, when its body carries one, or the status.
/// Every error the server sends is `{"message":..., "name":..., "statusCode":...}`
/// (server/src/errors.rs); `name` is the machine-readable half.
[[nodiscard]] std::string messageOf(const HttpResponse& response) {
    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_object()) {
        if (const auto it = body.find("message"); it != body.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return "HTTP " + std::to_string(response.status);
}

/// A string field, or empty when absent or null. The API sends `null` rather
/// than leaving a key out -- an instrumental has `"plainLyrics": null` -- and
/// nlohmann's `value()` throws on a null of the wrong type rather than
/// defaulting.
[[nodiscard]] std::string stringOf(const nlohmann::json& object, const char* key) {
    if (const auto it = object.find(key); it != object.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

}  // namespace

LrclibClient::LrclibClient(IHttpClient& http, std::string apiRoot)
    : http_(http), apiRoot_(std::move(apiRoot)) {}

void LrclibClient::setApiRoot(std::string apiRoot) {
    std::lock_guard lock(rootMutex_);
    apiRoot_ = std::move(apiRoot);
}

std::string LrclibClient::apiRoot() const {
    std::lock_guard lock(rootMutex_);
    return apiRoot_;
}

std::string LrclibClient::endpoint(std::string_view name) const {
    std::string root = apiRoot();
    if (root.empty()) {
        root = kDefaultApiRoot;
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    // "https://host" and "https://host/api" are both written down as the root
    // by people who have read different pages; the prefix is added only when
    // it is not already there.
    if (!root.ends_with("/api")) {
        root += "/api";
    }
    root += '/';
    root += name;
    return root;
}

std::string LrclibClient::clientHeader() {
    return "XPCog/" + std::string{kVersionString} + " (https://github.com/losnoco/XPCog)";
}

std::optional<LrclibLyrics> LrclibClient::get(const LyricsQuery& query, LyricsError* error) {
    if (query.title.empty() || query.artist.empty()) {
        setError(error, LyricsError::Kind::Incomplete, 0, "no title or no artist");
        return std::nullopt;
    }

    HttpParams params;
    params.emplace_back("track_name", query.title);
    params.emplace_back("artist_name", query.artist);
    if (!query.album.empty()) {
        params.emplace_back("album_name", query.album);
    }
    // Whole seconds: the server matches to within two, and rounds the value to
    // build its own cache key, so decimals would only make the URL longer. Out
    // of the range the server validates against, it is left out -- a request
    // with `duration=0` is refused as a whole rather than matched loosely.
    const auto seconds = std::llround(query.duration);
    if (seconds >= 1 && seconds <= 3600) {
        params.emplace_back("duration", std::to_string(seconds));
    }

    const HttpResponse response =
        http_.get(endpoint("get"), params, {{"Lrclib-Client", clientHeader()}});

    if (response.transportFailed()) {
        setError(error, LyricsError::Kind::Transport, 0, response.error);
        return std::nullopt;
    }
    const int status = static_cast<int>(response.status);
    if (status == 404) {
        setError(error, LyricsError::Kind::NotFound, status, messageOf(response));
        return std::nullopt;
    }
    if (status == 429 || status >= 500) {
        setError(error, LyricsError::Kind::Transient, status, messageOf(response));
        return std::nullopt;
    }
    if (status != 200) {
        setError(error, LyricsError::Kind::Api, status, messageOf(response));
        return std::nullopt;
    }

    const nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || !body.contains("id")) {
        setError(error, LyricsError::Kind::Malformed, status, "could not parse the reply");
        return std::nullopt;
    }

    LrclibLyrics lyrics;
    lyrics.plain  = stringOf(body, "plainLyrics");
    lyrics.synced = stringOf(body, "syncedLyrics");
    if (const auto it = body.find("instrumental"); it != body.end() && it->is_boolean()) {
        lyrics.instrumental = it->get<bool>();
    }
    clearError(error);
    return lyrics;
}

}  // namespace xpcog
