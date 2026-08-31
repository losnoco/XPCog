#include "Routes.hpp"

#include "Json.hpp"

#include "xpcog/core/Version.hpp"

#include <array>
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

RawResponse getStatus(const Ctx& ctx) {
    const auto status = gated(ctx, [](IPlayerControl& player) { return player.status(); });
    if (!status) {
        return interfaceBusy();
    }
    return jsonResponse(toJson(*status));
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

// --- parameter and schema tables -------------------------------------------

constexpr std::array<Param, 0> kNoParams{};

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

constexpr std::array kSchemas = std::to_array<Schema>({
    {"Error", schemaError},
    {"Status", schemaStatus},
    {"Version", schemaVersion},
    {"Volume", schemaVolume},
    {"Order", schemaOrder},
    {"PlayRequest", schemaPlay},
    {"SeekRequest", schemaSeek},
});

constexpr std::array kRoutes = std::to_array<Route>({
    {Method::Get, "/api/v1/version", "getVersion", "The player's version.",
     kNoParams, "", "Version", "application/json", false, false, getVersion},

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
