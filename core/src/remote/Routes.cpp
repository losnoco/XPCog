#include "Routes.hpp"

#include "Gzip.hpp"
#include "Json.hpp"

#include "xpcog/core/audio/Equalizer.hpp"

#include "swagger_resources.hpp"

#include "xpcog/core/Version.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace xpcog::remote {
namespace {

// --- shared shapes ---------------------------------------------------------

/// Runs `job` through the gate and turns "no answer" into 503 once, here,
/// rather than at every call site.
template <typename F>
auto gated(const Ctx& ctx, F job) {
    return ctx.gate.call(std::move(job));
}

/// Every command answers with the status afterwards. A client that has just
/// asked for `next` wants to know what is playing now, and making it ask again
/// is a second round trip for something this call already knows.
RawResponse statusAfter(const Ctx& ctx, Outcome outcome) {
    if (outcome != Outcome::Ok) {
        return outcomeResponse(outcome, {});
    }
    const auto status = gated(ctx, [](IPlayerControl& player) { return player.status(); });
    if (!status) {
        return interfaceBusy();
    }
    return jsonResponse(toJson(*status));
}

/// A command with no arguments, which is most of the transport.
template <Outcome (IPlayerControl::*Command)()>
RawResponse simpleCommand(const Ctx& ctx) {
    const auto outcome =
        gated(ctx, [](IPlayerControl& player) { return (player.*Command)(); });
    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

// --- handlers --------------------------------------------------------------

RawResponse getVersion(const Ctx& /*ctx*/) {
    nlohmann::json body;
    body["version"]    = kVersionString;
    body["apiVersion"] = 1;
    return jsonResponse(body);
}

// --- the docs page ---------------------------------------------------------
//
// These three are the only unauthenticated things the server has, and that is
// forced rather than chosen: a browser cannot put an Authorization header on a
// top-level navigation, so a token-gated /docs is a page nobody can open. What
// is exposed by that is three static blobs describing the page's own chrome --
// the specification and every endpoint still need the token, and the page asks
// for one and attaches it itself.
//
// The residual risk is worth naming: a hostile page could, through DNS
// rebinding, fetch these three files. It can reach nothing else, because there
// are no CORS headers anywhere and no ambient credential to borrow.
/// Does this request say it can read gzip?
///
/// Asked rather than assumed. The two big assets are committed compressed, and
/// sending Content-Encoding: gzip to a client that never offered to accept it is
/// a body it cannot read -- which is not hypothetical: cpp-httplib's own client
/// is built without zlib in some configurations, offers no Accept-Encoding, and
/// fails the response outright.
bool acceptsGzip(const RawRequest& request) {
    return request.acceptEncoding.find("gzip") != std::string::npos;
}

RawResponse serveDocsAsset(const Ctx& ctx, std::string_view name,
                           std::string_view type, bool compressed) {
    const std::span<const std::byte> bytes = resources::swagger(name);
    if (bytes.empty()) {
        // The embedder answers empty for a path it does not have, which is the
        // cue that a file was renamed without the CMake list following.
        return jsonError(404, "not_found", "That asset is not in this build.");
    }

    RawResponse response;
    response.contentType = std::string{type};

    if (compressed && !acceptsGzip(ctx.request)) {
        // Expanded here rather than refused. Every browser accepts gzip, so this
        // path is for the occasional script or client library that does not, and
        // answering 406 would make those the one kind of client that cannot read
        // the documentation.
        std::string plain;
        if (!inflateGzip(bytes, plain)) {
            return jsonError(500, "internal", "That asset could not be expanded.");
        }
        response.body = std::move(plain);
    } else {
        response.body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (compressed) {
            response.headers.emplace_back("Content-Encoding", "gzip");
        }
    }
    // The page may load its own two files and talk to this server, and nothing
    // else at all. 'unsafe-inline' is for styles only, because SwaggerUIBundle
    // injects them at runtime and there is no nonce to give it.
    response.headers.emplace_back(
        "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data:; connect-src 'self'; base-uri 'none'; form-action 'none'");
    return response;
}

RawResponse getDocs(const Ctx& ctx) {
    return serveDocsAsset(ctx, "index.html", "text/html; charset=utf-8", false);
}

RawResponse getDocsCss(const Ctx& ctx) {
    return serveDocsAsset(ctx, "swagger-ui.css.gz", "text/css; charset=utf-8", true);
}

RawResponse getDocsApp(const Ctx& ctx) {
    // Ours, not upstream's, and uncompressed because it is three kilobytes and
    // stays readable in the tree.
    return serveDocsAsset(ctx, "docs.js", "text/javascript; charset=utf-8", false);
}

RawResponse getDocsScript(const Ctx& ctx) {
    return serveDocsAsset(ctx, "swagger-ui-bundle.js.gz",
                          "text/javascript; charset=utf-8", true);
}

RawResponse getOpenApi(const Ctx& /*ctx*/) {
    RawResponse response;
    response.body        = RemoteServer::openApiDocument();
    response.contentType = "application/json; charset=utf-8";
    return response;
}

RawResponse getStatus(const Ctx& ctx) {
    const auto status = gated(ctx, [](IPlayerControl& player) { return player.status(); });
    if (!status) {
        return interfaceBusy();
    }
    return jsonResponse(toJson(*status));
}

/// What is playing, in one request.
///
/// A now-playing display is the commonest thing anybody builds against a player,
/// and without this it takes two calls -- /status for an id, then /playlist/{id}
/// for the title -- plus the branch for "nothing is playing" in between.
///
/// One hop, deliberately: asking the gate twice would let a track change land
/// between the two, and the id from the first answer would then miss in the
/// second. The client would see "playing, no track", which is not a state the
/// player is ever in.
RawResponse getNowPlaying(const Ctx& ctx) {
    struct Snapshot {
        Status                     status;
        std::optional<TrackDetail> track;
    };

    const auto snapshot = ctx.gate.call([](IPlayerControl& player) {
        Snapshot taken;
        taken.status = player.status();
        if (taken.status.currentTrack != kInvalidTrackId) {
            taken.track = player.track(taken.status.currentTrack);
        }
        return taken;
    });
    if (!snapshot) {
        return interfaceBusy();
    }

    // 200 with a null track rather than 404. "Nothing is playing" is a real
    // answer to this question, not a missing resource, and a display polling it
    // should not have to treat the ordinary idle case as an error.
    nlohmann::json body;
    body["playing"]  = snapshot->status.playing;
    body["paused"]   = snapshot->status.paused;
    body["position"] = snapshot->status.position;
    body["duration"] = snapshot->status.duration;
    body["track"]    = snapshot->track ? toJson(*snapshot->track) : nlohmann::json{};
    return jsonResponse(body);
}

RawResponse postPlay(const Ctx& ctx) {
    std::optional<TrackId> id;
    if (ctx.body.is_object()) {
        const auto found = ctx.body.find("trackId");
        if (found != ctx.body.end() && !found->is_null()) {
            if (!found->is_number_unsigned()) {
                return jsonError(400, "bad_request", "trackId must be a track id.",
                                 "trackId");
            }
            id = found->get<TrackId>();
        }
    }
    const auto outcome =
        gated(ctx, [id](IPlayerControl& player) { return player.play(id); });
    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

RawResponse postSeek(const Ctx& ctx) {
    // Seconds or a fraction of the track. The fraction is what a scrub bar has,
    // and making it work out the seconds means it needs the duration first --
    // one more round trip for something this end already knows.
    const std::optional<double> seconds  = readNumber(ctx.body, "seconds");
    const std::optional<double> fraction = readNumber(ctx.body, "fraction");

    if (!seconds && !fraction) {
        return jsonError(400, "bad_request", "Give either seconds or fraction.",
                         "seconds");
    }
    if (seconds && fraction) {
        return jsonError(400, "bad_request", "Give seconds or fraction, not both.",
                         "seconds");
    }
    if (fraction && (*fraction < 0.0 || *fraction > 1.0)) {
        return jsonError(400, "bad_request", "fraction must be between 0 and 1.",
                         "fraction");
    }
    if (seconds && *seconds < 0.0) {
        return jsonError(400, "bad_request", "seconds must not be negative.", "seconds");
    }

    std::optional<Outcome> outcome;
    if (seconds) {
        outcome = gated(ctx, [target = *seconds](IPlayerControl& player) {
            return player.seek(target);
        });
    } else {
        const auto status =
            gated(ctx, [](IPlayerControl& player) { return player.status(); });
        if (!status) {
            return interfaceBusy();
        }
        if (status->duration <= 0.0) {
            return jsonError(409, "not_seekable",
                             "Nothing with a known duration is playing.");
        }
        outcome = gated(ctx, [target = *fraction * status->duration](
                                 IPlayerControl& player) { return player.seek(target); });
    }

    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

RawResponse getVolume(const Ctx& ctx) {
    const auto status = gated(ctx, [](IPlayerControl& player) { return player.status(); });
    if (!status) {
        return interfaceBusy();
    }
    nlohmann::json body;
    body["volume"] = status->volume;
    return jsonResponse(body);
}

RawResponse putVolume(const Ctx& ctx) {
    const std::optional<double> volume = readNumber(ctx.body, "volume");
    if (!volume) {
        return jsonError(400, "bad_request", "volume must be a number.", "volume");
    }
    // Clamped by the player rather than here would mean 1.5 quietly becoming 1.0
    // and the response agreeing with neither the request nor the speakers.
    if (*volume < 0.0 || *volume > 1.0) {
        return jsonError(400, "bad_request", "volume must be between 0 and 1.", "volume");
    }
    const auto outcome = gated(ctx, [target = *volume](IPlayerControl& player) {
        return player.setVolume(target);
    });
    if (!outcome) {
        return interfaceBusy();
    }
    if (*outcome != Outcome::Ok) {
        return outcomeResponse(*outcome, {});
    }
    nlohmann::json body;
    body["volume"] = *volume;
    return jsonResponse(body);
}

RawResponse getOrder(const Ctx& ctx) {
    const auto status = gated(ctx, [](IPlayerControl& player) { return player.status(); });
    if (!status) {
        return interfaceBusy();
    }
    nlohmann::json body;
    body["repeat"]           = status->repeat;
    body["shuffle"]          = status->shuffle;
    body["stopAfterCurrent"] = status->stopAfterCurrent;
    return jsonResponse(body);
}

RawResponse putOrder(const Ctx& ctx) {
    std::optional<std::string> repeat  = readString(ctx.body, "repeat");
    std::optional<std::string> shuffle = readString(ctx.body, "shuffle");
    std::optional<bool>        stopAfter = readBool(ctx.body, "stopAfterCurrent");

    if (!repeat && !shuffle && !stopAfter) {
        return jsonError(400, "bad_request",
                         "Give at least one of repeat, shuffle or stopAfterCurrent.");
    }

    const auto outcome = gated(ctx, [&](IPlayerControl& player) {
        return player.setOrder(repeat, shuffle, stopAfter);
    });
    if (!outcome) {
        return interfaceBusy();
    }
    if (*outcome != Outcome::Ok) {
        return outcomeResponse(*outcome, {});
    }
    return getOrder(ctx);
}


// --- playlist --------------------------------------------------------------

/// A page of the playlist. Capped, because a client that asks for everything on
/// a hundred-thousand-row playlist would hold the interface thread for the whole
/// serialisation and then send it all.
constexpr std::size_t kDefaultLimit = 100;
constexpr std::size_t kMaxLimit     = 1000;

std::optional<std::size_t> readSize(const Query& query, std::string_view name,
                                    bool& bad) {
    const std::optional<std::string> text = query.get(name);
    if (!text) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(*text, &consumed);
        if (consumed != text->size() || value < 0) {
            bad = true;
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        bad = true;
        return std::nullopt;
    }
}

RawResponse getPlaylist(const Ctx& ctx) {
    bool                             bad    = false;
    const std::optional<std::size_t> offset = readSize(ctx.query, "offset", bad);
    const std::optional<std::size_t> limit  = readSize(ctx.query, "limit", bad);
    if (bad) {
        return jsonError(400, "bad_request", "offset and limit must be whole numbers.");
    }

    const std::size_t start = offset.value_or(0);
    const std::size_t count = std::min(limit.value_or(kDefaultLimit), kMaxLimit);
    const std::string needle = ctx.query.get("q").value_or(std::string{});

    const auto page = ctx.gate.call([&](IPlayerControl& player) {
        return player.tracks(start, count, needle);
    });
    if (!page) {
        return interfaceBusy();
    }

    nlohmann::json items = nlohmann::json::array();
    for (const TrackSummary& track : page->items) {
        items.push_back(toJson(track));
    }

    nlohmann::json body;
    body["total"]  = page->total;
    body["offset"] = start;
    body["limit"]  = count;
    // Null rather than absent when there is nothing playing, so a client reads
    // one field either way instead of testing for the key.
    body["currentRow"] = page->currentRow ? nlohmann::json(*page->currentRow)
                                          : nlohmann::json{};
    body["items"]  = std::move(items);
    return jsonResponse(body);
}

/// The `{id}` segment as a track id, or nothing when it is not one.
std::optional<TrackId> readTrackId(const Ctx& ctx) {
    const std::string text = ctx.pathParam("id");
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return static_cast<TrackId>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

RawResponse getTrack(const Ctx& ctx) {
    const std::optional<TrackId> id = readTrackId(ctx);
    if (!id) {
        return jsonError(400, "bad_request", "The track id is not a number.", "id");
    }
    const auto detail =
        ctx.gate.call([id](IPlayerControl& player) { return player.track(*id); });
    if (!detail) {
        return interfaceBusy();
    }
    if (!detail->has_value()) {
        return jsonError(404, "not_found", "No track with that id.");
    }
    return jsonResponse(toJson(**detail));
}

/// The cover art, as the file carried it.
///
/// Content-addressed in the library, so the hash makes a genuine ETag and a
/// client that already has the picture is answered with 304 rather than five
/// megabytes.
RawResponse getArtwork(const Ctx& ctx) {
    const std::optional<TrackId> id = readTrackId(ctx);
    if (!id) {
        return jsonError(400, "bad_request", "The track id is not a number.", "id");
    }
    const auto bytes =
        ctx.gate.call([id](IPlayerControl& player) { return player.artwork(*id); });
    if (!bytes) {
        return interfaceBusy();
    }
    if (*bytes == nullptr || (*bytes)->empty()) {
        return jsonError(404, "not_found", "That track has no cover art.");
    }

    RawResponse response;
    const std::vector<std::byte>& data = **bytes;
    response.body.assign(reinterpret_cast<const char*>(data.data()), data.size());

    // Sniffed rather than stored: the library keeps the bytes the file carried
    // and not what they were called. Two magic numbers cover everything a tag
    // actually holds.
    response.contentType = "application/octet-stream";
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8) {
        response.contentType = "image/jpeg";
    } else if (data.size() >= 8 && static_cast<unsigned char>(data[0]) == 0x89 &&
               static_cast<unsigned char>(data[1]) == 'P') {
        response.contentType = "image/png";
    }
    return response;
}

RawResponse postTracks(const Ctx& ctx) {
    const auto found = ctx.body.find("urls");
    if (found == ctx.body.end() || !found->is_array()) {
        return jsonError(400, "bad_request", "urls must be an array.", "urls");
    }
    std::vector<std::string> urls;
    urls.reserve(found->size());
    for (const nlohmann::json& element : *found) {
        if (!element.is_string()) {
            return jsonError(400, "bad_request", "urls must be strings.", "urls");
        }
        urls.push_back(element.get<std::string>());
    }
    if (urls.empty()) {
        return jsonError(400, "bad_request", "urls is empty.", "urls");
    }

    std::optional<std::size_t> at;
    const auto                 where = ctx.body.find("at");
    if (where != ctx.body.end() && !where->is_null()) {
        if (!where->is_number_unsigned()) {
            return jsonError(400, "bad_request", "at must be a position.", "at");
        }
        at = where->get<std::size_t>();
    }

    const auto job = ctx.gate.call([&](IPlayerControl& player) {
        return player.addUrls(urls, at);
    });
    if (!job) {
        return interfaceBusy();
    }
    if (job->empty()) {
        return jsonError(501, "unsupported", "This host cannot add tracks.");
    }

    // 202, not 200. A directory of ten thousand files is not something to hold a
    // request open for, and the gate would time out long before the scan
    // finished. The job id is how the client follows it.
    nlohmann::json body;
    body["jobId"] = *job;
    RawResponse response = jsonResponse(body, 202);
    response.headers.emplace_back("Location", "/api/v1/jobs/" + *job);
    return response;
}

/// The body shape shared by everything that names a set of tracks.
std::optional<std::vector<TrackId>> requireIds(const Ctx& ctx, RawResponse& error) {
    std::optional<std::vector<TrackId>> ids = readIds(ctx.body, "ids");
    if (!ids) {
        error = jsonError(400, "bad_request", "ids must be an array of track ids.",
                          "ids");
        return std::nullopt;
    }
    if (ids->empty()) {
        error = jsonError(400, "bad_request", "ids is empty.", "ids");
        return std::nullopt;
    }
    return ids;
}

RawResponse deleteTracks(const Ctx& ctx) {
    RawResponse error;
    const auto  ids = requireIds(ctx, error);
    if (!ids) {
        return error;
    }
    const auto outcome = ctx.gate.call(
        [&](IPlayerControl& player) { return player.removeTracks(*ids); });
    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

RawResponse postMove(const Ctx& ctx) {
    RawResponse error;
    const auto  ids = requireIds(ctx, error);
    if (!ids) {
        return error;
    }
    // null means the end, which is what a drop below the last row is.
    TrackId    anchor = kInvalidTrackId;
    const auto before = ctx.body.find("before");
    if (before != ctx.body.end() && !before->is_null()) {
        if (!before->is_number_unsigned()) {
            return jsonError(400, "bad_request", "before must be a track id or null.",
                             "before");
        }
        anchor = before->get<TrackId>();
    }

    const auto outcome = ctx.gate.call(
        [&](IPlayerControl& player) { return player.moveTracks(*ids, anchor); });
    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

RawResponse postQueue(const Ctx& ctx) {
    RawResponse error;
    const auto  ids = requireIds(ctx, error);
    if (!ids) {
        return error;
    }
    const bool queued = readBool(ctx.body, "queued").value_or(true);
    const auto outcome = ctx.gate.call(
        [&](IPlayerControl& player) { return player.setQueued(*ids, queued); });
    if (!outcome) {
        return interfaceBusy();
    }
    return statusAfter(ctx, *outcome);
}

RawResponse patchTrack(const Ctx& ctx) {
    const std::optional<TrackId> id = readTrackId(ctx);
    if (!id) {
        return jsonError(400, "bad_request", "The track id is not a number.", "id");
    }
    const std::vector<TrackId> ids{*id};

    // Per-entry flags, and deliberately not undoable -- none of them is undoable
    // from the window either, and widening that here would be a behaviour change
    // smuggled in through the API.
    bool touched = false;
    if (const std::optional<bool> stopAfter = readBool(ctx.body, "stopAfter")) {
        touched = true;
        const auto outcome = ctx.gate.call([&](IPlayerControl& player) {
            return player.setStopAfter(ids, *stopAfter);
        });
        if (!outcome) {
            return interfaceBusy();
        }
        if (*outcome != Outcome::Ok) {
            return outcomeResponse(*outcome, {});
        }
    }
    if (ctx.body.contains("rating")) {
        touched = true;
        std::optional<double> rating;
        if (!ctx.body.at("rating").is_null()) {
            rating = readNumber(ctx.body, "rating");
            if (!rating || *rating < 0.0 || *rating > 5.0) {
                return jsonError(400, "bad_request",
                                 "rating must be null or between 0 and 5.", "rating");
            }
        }
        const auto outcome = ctx.gate.call(
            [&](IPlayerControl& player) { return player.setRating(ids, rating); });
        if (!outcome) {
            return interfaceBusy();
        }
        if (*outcome != Outcome::Ok) {
            return outcomeResponse(*outcome, {});
        }
    }
    if (const std::optional<double> playCount = readNumber(ctx.body, "playCount")) {
        if (*playCount != 0.0) {
            // Setting a play count to an arbitrary number is not something the
            // window can do either, and inventing listening history is not what
            // this endpoint is for.
            return jsonError(400, "bad_request", "playCount can only be set to 0.",
                             "playCount");
        }
        touched = true;
        const auto outcome = ctx.gate.call(
            [&](IPlayerControl& player) { return player.resetPlayCount(ids); });
        if (!outcome) {
            return interfaceBusy();
        }
        if (*outcome != Outcome::Ok) {
            return outcomeResponse(*outcome, {});
        }
    }

    if (!touched) {
        return jsonError(400, "bad_request",
                         "Give at least one of stopAfter, rating or playCount.");
    }
    return getTrack(ctx);
}

RawResponse getJob(const Ctx& ctx) {
    const std::string id = ctx.pathParam("id");
    const auto        job =
        ctx.gate.call([&](IPlayerControl& player) { return player.job(id); });
    if (!job) {
        return interfaceBusy();
    }
    if (!job->has_value()) {
        return jsonError(404, "not_found", "No such job.");
    }
    return jsonResponse(toJson(**job));
}


// --- settings and DSP ------------------------------------------------------

RawResponse getSettings(const Ctx& ctx) {
    const auto all =
        ctx.gate.call([](IPlayerControl& player) { return player.settings(); });
    if (!all) {
        return interfaceBusy();
    }
    nlohmann::json items = nlohmann::json::array();
    for (const SettingInfo& setting : *all) {
        items.push_back(toJson(setting));
    }
    nlohmann::json body;
    body["items"] = std::move(items);
    return jsonResponse(body);
}

RawResponse getSetting(const Ctx& ctx) {
    const std::string key = ctx.pathParam("key");
    const auto        found =
        ctx.gate.call([&](IPlayerControl& player) { return player.setting(key); });
    if (!found) {
        return interfaceBusy();
    }
    if (!found->has_value()) {
        return jsonError(404, "not_found", "No setting with that key.");
    }
    return jsonResponse(toJson(**found));
}

RawResponse putSetting(const Ctx& ctx) {
    const std::string key = ctx.pathParam("key");

    // A string, whatever the setting's type. settings.def has four scalar types
    // and Settings stores every one of them as text; taking a JSON number for an
    // int and a JSON string for a string would make the wire shape depend on a
    // type the client has to look up first.
    const std::optional<std::string> value = readString(ctx.body, "value");
    if (!value) {
        return jsonError(400, "bad_request",
                         "value must be a string; every setting is stored as text.",
                         "value");
    }

    const auto write = ctx.gate.call(
        [&](IPlayerControl& player) { return player.setSetting(key, *value); });
    if (!write) {
        return interfaceBusy();
    }
    if (write->outcome != Outcome::Ok) {
        // ReadOnly is the interesting one: session state is readable and not
        // writable, which is the rule the Advanced pane already applies.
        RawResponse response = outcomeResponse(write->outcome, {});
        if (!write->message.empty()) {
            nlohmann::json body = nlohmann::json::parse(response.body, nullptr, false);
            if (!body.is_discarded()) {
                body["error"]["message"] = write->message;
                response.body            = body.dump();
            }
        }
        return response;
    }

    nlohmann::json body;
    body["key"]   = key;
    body["value"] = *value;
    // Never omitted, and never guessed at the call site: an equaliser band moves
    // what is playing and a ReplayGain mode does not, and an API that reported
    // both as done would be wrong about one of them.
    body["appliesFrom"] = write->appliesFrom;
    return jsonResponse(body);
}

RawResponse patchSettings(const Ctx& ctx) {
    if (ctx.body.empty()) {
        return jsonError(400, "bad_request", "No settings given.");
    }
    // One hop for the lot. Twenty separate writes would be twenty round trips
    // and twenty fan-outs, and a preferences screen saving itself is exactly the
    // caller this is for.
    struct Write {
        std::string key;
        std::string value;
    };
    std::vector<Write> writes;
    for (const auto& [key, value] : ctx.body.items()) {
        if (!value.is_string()) {
            return jsonError(400, "bad_request",
                             "Every value must be a string.", key);
        }
        writes.push_back({key, value.get<std::string>()});
    }

    const auto results = ctx.gate.call([&](IPlayerControl& player) {
        std::vector<std::pair<std::string, SettingWrite>> answers;
        answers.reserve(writes.size());
        for (const Write& write : writes) {
            answers.emplace_back(write.key, player.setSetting(write.key, write.value));
        }
        return answers;
    });
    if (!results) {
        return interfaceBusy();
    }

    // Reported per key rather than as one verdict: a batch where one key was
    // read-only and the rest took is not a failure, and it is not a success
    // either.
    nlohmann::json items = nlohmann::json::array();
    bool           anyFailed = false;
    for (const auto& [key, write] : *results) {
        nlohmann::json item;
        item["key"]         = key;
        item["ok"]          = write.outcome == Outcome::Ok;
        item["appliesFrom"] = write.appliesFrom;
        if (write.outcome != Outcome::Ok) {
            anyFailed = true;
            item["message"] = write.message;
        }
        items.push_back(std::move(item));
    }

    nlohmann::json body;
    body["results"] = std::move(items);
    return jsonResponse(body, anyFailed ? 207 : 200);
}

RawResponse getEqualizer(const Ctx& ctx) {
    const auto state =
        ctx.gate.call([](IPlayerControl& player) { return player.equalizer(); });
    if (!state) {
        return interfaceBusy();
    }
    return jsonResponse(toJson(*state));
}

RawResponse putEqualizer(const Ctx& ctx) {
    const std::optional<bool>   enabled = readBool(ctx.body, "enabled");
    const std::optional<double> preamp  = readNumber(ctx.body, "preamp");

    std::vector<std::pair<double, double>> bands;
    const auto                             found = ctx.body.find("bands");
    if (found != ctx.body.end()) {
        if (!found->is_array()) {
            return jsonError(400, "bad_request", "bands must be an array.", "bands");
        }
        const std::span<const double> frequencies = Equalizer::bandFrequencies();
        for (const nlohmann::json& band : *found) {
            const std::optional<double> hz   = readNumber(band, "hz");
            const std::optional<double> gain = readNumber(band, "gain");
            if (!hz || !gain) {
                return jsonError(400, "bad_request",
                                 "Every band needs hz and gain.", "bands");
            }
            // Checked here rather than only in the player, so the answer can say
            // which value was wrong and what the alternatives are. The bands are
            // a fixed table, and a client that guessed 32 for 31.5 otherwise gets
            // "the player refused that value" and no way to find out why.
            if (std::find(frequencies.begin(), frequencies.end(), *hz) ==
                frequencies.end()) {
                // Trimmed, because std::to_string on a double gives six decimal
                // places and "No band at 32.000000 Hz" reads as a rounding
                // problem rather than a wrong number.
                std::string shown = std::to_string(*hz);
                if (shown.find('.') != std::string::npos) {
                    shown.erase(shown.find_last_not_of('0') + 1);
                    if (shown.back() == '.') {
                        shown.pop_back();
                    }
                }
                const std::string message =
                    "No band at " + shown + " Hz. GET this endpoint for the " +
                    std::to_string(frequencies.size()) + " frequencies there are.";
                return jsonError(400, "unknown_band", message, "bands");
            }
            bands.emplace_back(*hz, *gain);
        }
    }

    if (!enabled && !preamp && bands.empty()) {
        return jsonError(400, "bad_request",
                         "Give at least one of enabled, preamp or bands.");
    }

    const auto outcome = ctx.gate.call([&](IPlayerControl& player) {
        return player.setEqualizer(enabled, preamp, bands);
    });
    if (!outcome) {
        return interfaceBusy();
    }
    if (*outcome != Outcome::Ok) {
        return outcomeResponse(*outcome, {});
    }
    return getEqualizer(ctx);
}

RawResponse getPresets(const Ctx& ctx) {
    const auto names =
        ctx.gate.call([](IPlayerControl& player) { return player.equalizerPresets(); });
    if (!names) {
        return interfaceBusy();
    }
    nlohmann::json body;
    body["items"] = *names;
    return jsonResponse(body);
}

RawResponse postPreset(const Ctx& ctx) {
    const std::optional<std::string> name = readString(ctx.body, "name");
    if (!name || name->empty()) {
        return jsonError(400, "bad_request", "name must be a preset name.", "name");
    }
    const auto outcome = ctx.gate.call([&](IPlayerControl& player) {
        return player.applyEqualizerPreset(*name);
    });
    if (!outcome) {
        return interfaceBusy();
    }
    if (*outcome != Outcome::Ok) {
        return outcomeResponse(*outcome, {});
    }
    return getEqualizer(ctx);
}

// --- parameter and schema tables -------------------------------------------

constexpr std::array<Param, 0> kNoParams{};

constexpr std::array kPlaylistParams = std::to_array<Param>({
    {"offset", In::Query, "integer", false, "Row to start at. Default 0."},
    {"limit", In::Query, "integer", false, "Rows to return. Default 100, max 1000."},
    {"q", In::Query, "string", false,
     "Substring across title, artist and album, case-insensitive. Does not change "
     "what the player's own filter box is showing."},
});

constexpr std::array kTrackParams = std::to_array<Param>({
    {"id", In::Path, "integer", true, "A track id from GET /playlist."},
});

constexpr std::array kJobParams = std::to_array<Param>({
    {"id", In::Path, "string", true, "A job id from POST /playlist/tracks."},
});

constexpr std::array kSettingParams = std::to_array<Param>({
    {"key", In::Path, "string", true, "A settings key, as GET /settings lists them."},
});

nlohmann::json schemaError() {
    nlohmann::json inner;
    inner["type"]       = "object";
    inner["required"]   = nlohmann::json::array({"code", "message"});
    inner["properties"] = {
        {"code", {{"type", "string"}, {"description", "A stable machine-readable name."}}},
        {"message", {{"type", "string"}}},
        {"field", {{"type", "string"}}}};

    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"error"});
    schema["properties"] = {{"error", inner}};
    return schema;
}

nlohmann::json schemaStatus() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"playing", {{"type", "boolean"}}},
        {"paused", {{"type", "boolean"}}},
        {"currentTrack", {{"type", nlohmann::json::array({"integer", "null"})}}},
        {"position", {{"type", "number"}, {"description", "Seconds."}}},
        {"duration", {{"type", "number"}, {"description", "Seconds; 0 when unknown."}}},
        {"volume", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
        {"repeat", {{"type", "string"},
                    {"enum", nlohmann::json::array({"none", "one", "album", "all"})}}},
        {"shuffle", {{"type", "string"},
                     {"enum", nlohmann::json::array({"off", "albums", "all"})}}},
        {"stopAfterCurrent", {{"type", "boolean"}}},
        {"playlistSize", {{"type", "integer"}}},
        {"playlistRevision",
         {{"type", "integer"},
          {"description",
           "Bumped on every playlist change. Track ids are scoped to a session: "
           "when this or sessionId moves, anything cached is stale."}}},
        {"sessionId", {{"type", "string"}}},
        {"apiVersion", {{"type", "integer"}}}};
    return schema;
}

nlohmann::json schemaNowPlaying() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"playing", {{"type", "boolean"}}},
        {"paused", {{"type", "boolean"}}},
        {"position", {{"type", "number"}, {"description", "Seconds."}}},
        {"duration", {{"type", "number"}, {"description", "Seconds; 0 when unknown."}}},
        {"track", {{"oneOf", nlohmann::json::array(
                                 {{{"$ref", "#/components/schemas/TrackDetail"}},
                                  {{"type", "null"}}})},
                   {"description", "Null when nothing is playing."}}}};
    return schema;
}

nlohmann::json schemaVersion() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {{"version", {{"type", "string"}}},
                            {"apiVersion", {{"type", "integer"}}}};
    return schema;
}

nlohmann::json schemaVolume() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"volume"});
    schema["properties"] = {
        {"volume", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}};
    return schema;
}

nlohmann::json schemaOrder() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"repeat", {{"type", "string"},
                    {"enum", nlohmann::json::array({"none", "one", "album", "all"})}}},
        {"shuffle", {{"type", "string"},
                     {"enum", nlohmann::json::array({"off", "albums", "all"})}}},
        {"stopAfterCurrent", {{"type", "boolean"}}}};
    return schema;
}

nlohmann::json schemaPlay() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"trackId", {{"type", nlohmann::json::array({"integer", "null"})},
                     {"description", "Omit to resume what is loaded."}}}};
    return schema;
}

nlohmann::json schemaSeek() {
    nlohmann::json schema;
    schema["type"]        = "object";
    schema["description"] = "Exactly one of seconds or fraction.";
    schema["properties"]  = {
        {"seconds", {{"type", "number"}, {"minimum", 0}}},
        {"fraction", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}};
    return schema;
}

nlohmann::json schemaTrack() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"id", {{"type", "integer"},
                {"description",
                 "Scoped to the session. See Status.playlistRevision."}}},
        {"url", {{"type", "string"}}},
        {"title", {{"type", "string"}}},
        {"artist", {{"type", "string"}}},
        {"album", {{"type", "string"}}},
        {"duration", {{"type", "number"}, {"description", "Seconds."}}},
        {"track", {{"type", "integer"}}},
        {"disc", {{"type", "integer"}}},
        {"playCount", {{"type", "integer"}}},
        {"queued", {{"type", "boolean"}}},
        {"stopAfter", {{"type", "boolean"}}},
        {"error", {{"type", "boolean"}}},
        {"hasArtwork", {{"type", "boolean"}}}};
    return schema;
}

nlohmann::json schemaTrackDetail() {
    nlohmann::json schema = schemaTrack();
    schema["properties"]["albumArtist"]  = {{"type", "string"}};
    schema["properties"]["genre"]        = {{"type", "string"}};
    schema["properties"]["composer"]     = {{"type", "string"}};
    schema["properties"]["date"]         = {{"type", "string"}};
    schema["properties"]["comment"]      = {{"type", "string"}};
    schema["properties"]["lyrics"]       = {{"type", "string"}};
    schema["properties"]["errorMessage"] = {{"type", "string"}};
    schema["properties"]["properties"]   = {{"type", "object"}};
    schema["properties"]["metadata"]     = {
        {"type", "object"},
        {"description", "Every tag the file carried, including unpromoted ones."},
        {"additionalProperties", {{"type", "string"}}}};
    return schema;
}

nlohmann::json schemaTrackPage() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"total", {{"type", "integer"}, {"description", "How many matched."}}},
        {"offset", {{"type", "integer"}}},
        {"limit", {{"type", "integer"}}},
        {"currentRow",
         {{"type", nlohmann::json::array({"integer", "null"})},
          {"description",
           "Where the playing track sits in this filter and sort, counted over "
           "the whole match rather than over items -- so it says which page to "
           "ask for. Null when nothing is playing or the query excludes it."}}},
        {"items", {{"type", "array"},
                   {"items", {{"$ref", "#/components/schemas/Track"}}}}}};
    return schema;
}

nlohmann::json schemaIds() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"ids"});
    schema["properties"] = {
        {"ids", {{"type", "array"}, {"items", {{"type", "integer"}}}}}};
    return schema;
}

nlohmann::json schemaAddTracks() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"urls"});
    schema["properties"] = {
        {"urls", {{"type", "array"},
                  {"items", {{"type", "string"}}},
                  {"description",
                   "File, directory or stream URLs. A plain path is accepted."}}},
        {"at", {{"type", nlohmann::json::array({"integer", "null"})},
                {"description", "Row to insert before. Null or absent means the end."}}}};
    return schema;
}

nlohmann::json schemaMove() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"ids"});
    schema["properties"] = {
        {"ids", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
        {"before", {{"type", nlohmann::json::array({"integer", "null"})},
                    {"description", "Track to move in front of. Null means the end."}}}};
    return schema;
}

nlohmann::json schemaQueue() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"ids"});
    schema["properties"] = {
        {"ids", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
        {"queued", {{"type", "boolean"}, {"description", "Default true."}}}};
    return schema;
}

nlohmann::json schemaTrackPatch() {
    nlohmann::json schema;
    schema["type"]        = "object";
    schema["description"] = "Per-entry flags. Not undoable, as they are not in the "
                            "player's own window.";
    schema["properties"]  = {
        {"stopAfter", {{"type", "boolean"}}},
        {"rating", {{"type", nlohmann::json::array({"number", "null"})},
                    {"minimum", 0},
                    {"maximum", 5}}},
        {"playCount", {{"type", "integer"},
                       {"description", "Only 0, which resets it."}}}};
    return schema;
}

nlohmann::json schemaJob() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"id", {{"type", "string"}}},
        {"state", {{"type", "string"},
                   {"enum", nlohmann::json::array({"queued", "running", "done",
                                                   "failed"})}}},
        {"progress", {{"type", "object"}}},
        {"added", {{"type", "integer"}}},
        {"error", {{"type", nlohmann::json::array({"string", "null"})}}}};
    return schema;
}

nlohmann::json schemaSetting() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"key", {{"type", "string"}}},
        {"type", {{"type", "string"},
                  {"enum", nlohmann::json::array({"bool", "int", "double",
                                                  "std::string"})}}},
        {"value", {{"type", "string"},
                   {"description", "Every setting is stored and given as text."}}},
        {"default", {{"type", "string"}}},
        {"writable", {{"type", "boolean"},
                      {"description",
                       "False for session state, which is readable but not a "
                       "preference."}}},
        {"appliesFrom", {{"type", "string"},
                         {"enum", nlohmann::json::array({"immediately", "nextTrack",
                                                         "nextDeviceOpen", "nextScan",
                                                         "nextLaunch"})}}}};
    return schema;
}

nlohmann::json schemaSettingList() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"items", {{"type", "array"},
                   {"items", {{"$ref", "#/components/schemas/Setting"}}}}}};
    return schema;
}

nlohmann::json schemaSettingWrite() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"value"});
    schema["properties"] = {
        {"value", {{"type", "string"},
                   {"description", "Text, whatever the setting's type."}}}};
    return schema;
}

nlohmann::json schemaSettingWritten() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {{"key", {{"type", "string"}}},
                            {"value", {{"type", "string"}}},
                            {"appliesFrom", {{"type", "string"}}}};
    return schema;
}

nlohmann::json schemaSettingBatch() {
    nlohmann::json schema;
    schema["type"]        = "object";
    schema["description"] = "Keys to values, all text. Answers 207 when some key "
                            "was refused and the rest took.";
    schema["additionalProperties"] = {{"type", "string"}};
    return schema;
}

nlohmann::json schemaSettingBatchResult() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {{"results", {{"type", "array"},
                                         {"items", {{"type", "object"}}}}}};
    return schema;
}

nlohmann::json schemaEqualizer() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"enabled", {{"type", "boolean"}}},
        {"preamp", {{"type", "number"}, {"description", "dB."}}},
        {"trackGenre", {{"type", "boolean"}}},
        {"preset", {{"type", nlohmann::json::array({"string", "null"})},
                    {"description",
                     "The preset this curve is, or null when it is not one of "
                     "them. Applying a preset also switches the equaliser on, "
                     "unless it is Flat."}}},
        {"bands", {{"type", "array"},
                   {"items", {{"type", "object"}}},
                   {"description", "31 bands, each with hz and a gain in dB."}}},
        {"appliesFrom", {{"type", "string"},
                         {"description",
                          "Always immediately: the equaliser moves what is already "
                          "playing, unlike volumeScaling."}}}};
    return schema;
}

nlohmann::json schemaPreset() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["required"]   = nlohmann::json::array({"name"});
    schema["properties"] = {{"name", {{"type", "string"}}}};
    return schema;
}

nlohmann::json schemaPresetList() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {
        {"items", {{"type", "array"}, {"items", {{"type", "string"}}}}}};
    return schema;
}

nlohmann::json schemaJobAccepted() {
    nlohmann::json schema;
    schema["type"]       = "object";
    schema["properties"] = {{"jobId", {{"type", "string"}}}};
    return schema;
}

constexpr std::array kSchemas = std::to_array<Schema>({
    {"Error", schemaError},
    {"Status", schemaStatus},
    {"NowPlaying", schemaNowPlaying},
    {"Version", schemaVersion},
    {"Volume", schemaVolume},
    {"Order", schemaOrder},
    {"PlayRequest", schemaPlay},
    {"SeekRequest", schemaSeek},
    {"Track", schemaTrack},
    {"TrackDetail", schemaTrackDetail},
    {"TrackPage", schemaTrackPage},
    {"Ids", schemaIds},
    {"AddTracks", schemaAddTracks},
    {"Move", schemaMove},
    {"Queue", schemaQueue},
    {"TrackPatch", schemaTrackPatch},
    {"Job", schemaJob},
    {"JobAccepted", schemaJobAccepted},
    {"Setting", schemaSetting},
    {"SettingList", schemaSettingList},
    {"SettingWrite", schemaSettingWrite},
    {"SettingWritten", schemaSettingWritten},
    {"SettingBatch", schemaSettingBatch},
    {"SettingBatchResult", schemaSettingBatchResult},
    {"Equalizer", schemaEqualizer},
    {"Preset", schemaPreset},
    {"PresetList", schemaPresetList},
});

constexpr std::array kRoutes = std::to_array<Route>({
    {Method::Get, "/api/v1/version", "getVersion", "The player's version.",
     kNoParams, "", "Version", "application/json", false, false, getVersion},

    {Method::Get, "/docs", "getDocs",
     "A browser page for trying the API. Served without a token, because a "
     "browser cannot send one on a navigation; it asks for one itself.",
     kNoParams, "", "", "text/html", false, false, getDocs},

    {Method::Get, "/docs/swagger-ui.css", "getDocsStylesheet",
     "The docs page's stylesheet.", kNoParams, "", "", "text/css", false, false,
     getDocsCss},

    {Method::Get, "/docs/swagger-ui-bundle.js", "getDocsScript",
     "The docs page's script.", kNoParams, "", "", "text/javascript", false, false,
     getDocsScript},

    {Method::Get, "/docs/docs.js", "getDocsApp",
     "The docs page's own script, which asks for the token and attaches it.",
     kNoParams, "", "", "text/javascript", false, false, getDocsApp},

    {Method::Get, "/openapi.json", "getOpenApi",
     "This document. Needs a token like everything else.", kNoParams, "", "",
     "application/json", false, false, getOpenApi},

    {Method::Get, "/api/v1/nowplaying", "getNowPlaying",
     "What is playing, with its tags, in one request.", kNoParams, "",
     "NowPlaying", "application/json", false, false, getNowPlaying},

    {Method::Get, "/api/v1/status", "getStatus",
     "What is playing, where, and how the playlist is ordered.", kNoParams, "",
     "Status", "application/json", false, false, getStatus},

    {Method::Post, "/api/v1/transport/play", "play",
     "Plays a track, or resumes the loaded one.", kNoParams, "PlayRequest", "Status",
     "application/json", true, false, postPlay},

    {Method::Post, "/api/v1/transport/pause", "pause", "Pauses playback.", kNoParams,
     "", "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::pause>},

    {Method::Post, "/api/v1/transport/playPause", "playPause",
     "Pauses if playing, plays if paused.", kNoParams, "", "Status",
     "application/json", true, false, simpleCommand<&IPlayerControl::playPause>},

    {Method::Post, "/api/v1/transport/stop", "stop", "Stops playback.", kNoParams, "",
     "Status", "application/json", true, false, simpleCommand<&IPlayerControl::stop>},

    {Method::Post, "/api/v1/transport/next", "next", "Plays the next track.",
     kNoParams, "", "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::next>},

    {Method::Post, "/api/v1/transport/previous", "previous",
     "Plays the previous track.", kNoParams, "", "Status", "application/json", true,
     false, simpleCommand<&IPlayerControl::previous>},

    {Method::Post, "/api/v1/transport/seek", "seek",
     "Moves the playhead within the current track.", kNoParams, "SeekRequest",
     "Status", "application/json", true, false, postSeek},

    {Method::Get, "/api/v1/transport/volume", "getVolume", "The output gain.",
     kNoParams, "", "Volume", "application/json", false, false, getVolume},

    {Method::Put, "/api/v1/transport/volume", "setVolume", "Sets the output gain.",
     kNoParams, "Volume", "Volume", "application/json", true, false, putVolume},

    {Method::Get, "/api/v1/transport/order", "getOrder",
     "Repeat, shuffle and stop-after-current.", kNoParams, "", "Order",
     "application/json", false, false, getOrder},

    {Method::Put, "/api/v1/transport/order", "setOrder",
     "Sets any of repeat, shuffle and stop-after-current.", kNoParams, "Order",
     "Order", "application/json", true, false, putOrder},

    {Method::Get, "/api/v1/playlist", "getPlaylist",
     "One page of the playlist, optionally filtered.", kPlaylistParams, "",
     "TrackPage", "application/json", false, false, getPlaylist},

    {Method::Delete, "/api/v1/playlist", "clearPlaylist",
     "Removes every track. Undoable in the player's own window.", kNoParams, "",
     "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::clearPlaylist>},

    {Method::Get, "/api/v1/playlist/{id}", "getTrack",
     "One track, with its properties and every tag.", kTrackParams, "",
     "TrackDetail", "application/json", false, false, getTrack},

    {Method::Patch, "/api/v1/playlist/{id}", "patchTrack",
     "Sets stop-after, rating or play count on one track.", kTrackParams,
     "TrackPatch", "TrackDetail", "application/json", true, false, patchTrack},

    {Method::Get, "/api/v1/playlist/{id}/artwork", "getArtwork",
     "The cover art as the file carried it.", kTrackParams, "", "",
     "image/*", false, false, getArtwork},

    {Method::Post, "/api/v1/playlist/tracks", "addTracks",
     "Adds files, folders or streams. Answers a job to follow.", kNoParams,
     "AddTracks", "JobAccepted", "application/json", true, true, postTracks},

    {Method::Delete, "/api/v1/playlist/tracks", "removeTracks",
     "Removes tracks. Undoable in the player's own window.", kNoParams, "Ids",
     "Status", "application/json", true, false, deleteTracks},

    {Method::Post, "/api/v1/playlist/move", "moveTracks",
     "Moves tracks before another, or to the end.", kNoParams, "Move", "Status",
     "application/json", true, false, postMove},

    {Method::Post, "/api/v1/playlist/randomize", "randomize",
     "Shuffles the playlist for real. Undoable in the player's own window.",
     kNoParams, "", "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::randomize>},

    {Method::Post, "/api/v1/playlist/queue", "setQueued",
     "Adds tracks to the play queue, or takes them out.", kNoParams, "Queue",
     "Status", "application/json", true, false, postQueue},

    {Method::Delete, "/api/v1/playlist/queue", "clearQueue", "Empties the queue.",
     kNoParams, "", "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::clearQueue>},

    {Method::Post, "/api/v1/playlist/undo", "undo",
     "Undoes the last playlist edit, whoever made it.", kNoParams, "", "Status",
     "application/json", true, false, simpleCommand<&IPlayerControl::undo>},

    {Method::Post, "/api/v1/playlist/redo", "redo", "Redoes the last undone edit.",
     kNoParams, "", "Status", "application/json", true, false,
     simpleCommand<&IPlayerControl::redo>},

    {Method::Get, "/api/v1/jobs/{id}", "getJob",
     "How a scan started by POST /playlist/tracks is getting on.", kJobParams, "",
     "Job", "application/json", false, false, getJob},

    {Method::Get, "/api/v1/settings", "getSettings",
     "Every setting, with its value, default and when a write takes effect.",
     kNoParams, "", "SettingList", "application/json", false, false, getSettings},

    {Method::Patch, "/api/v1/settings", "patchSettings",
     "Writes several settings in one hop.", kNoParams, "SettingBatch",
     "SettingBatchResult", "application/json", true, false, patchSettings},

    {Method::Get, "/api/v1/settings/{key}", "getSetting", "One setting.",
     kSettingParams, "", "Setting", "application/json", false, false, getSetting},

    {Method::Put, "/api/v1/settings/{key}", "setSetting", "Writes one setting.",
     kSettingParams, "SettingWrite", "SettingWritten", "application/json", true,
     false, putSetting},

    {Method::Get, "/api/v1/dsp/equalizer", "getEqualizer",
     "The 31 bands, the preamp and the switch.", kNoParams, "", "Equalizer",
     "application/json", false, false, getEqualizer},

    {Method::Put, "/api/v1/dsp/equalizer", "setEqualizer",
     "Sets any of enabled, preamp and the bands.", kNoParams, "Equalizer",
     "Equalizer", "application/json", true, false, putEqualizer},

    {Method::Get, "/api/v1/dsp/equalizer/presets", "getEqualizerPresets",
     "The preset names this build ships.", kNoParams, "", "PresetList",
     "application/json", false, false, getPresets},

    {Method::Post, "/api/v1/dsp/equalizer/preset", "applyEqualizerPreset",
     "Applies a preset to the bands.", kNoParams, "Preset", "Equalizer",
     "application/json", true, false, postPreset},
});

}  // namespace

std::string Ctx::pathParam(std::string_view name) const {
    for (const auto& [key, value] : path) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::span<const Route>  routes() { return kRoutes; }
std::span<const Schema> schemas() { return kSchemas; }

std::string_view methodName(Method method) {
    switch (method) {
        case Method::Get:    return "GET";
        case Method::Post:   return "POST";
        case Method::Put:    return "PUT";
        case Method::Patch:  return "PATCH";
        case Method::Delete: return "DELETE";
    }
    return "GET";
}

}  // namespace xpcog::remote
