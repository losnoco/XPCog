#include "Json.hpp"

namespace xpcog::remote {

nlohmann::json toJson(const Status& status) {
    nlohmann::json out;
    out["playing"]          = status.playing;
    out["paused"]           = status.paused;
    // Null rather than 0 for "nothing is playing". Zero is not a track id, and a
    // client that treated it as one would be asking about a track that cannot
    // exist.
    out["currentTrack"]     = status.currentTrack == kInvalidTrackId
                                  ? nlohmann::json{}
                                  : nlohmann::json(status.currentTrack);
    out["position"]         = status.position;
    out["duration"]         = status.duration;
    out["volume"]           = status.volume;
    out["repeat"]           = status.repeat;
    out["shuffle"]          = status.shuffle;
    out["stopAfterCurrent"] = status.stopAfterCurrent;
    out["playlistSize"]     = status.playlistSize;
    out["playlistRevision"] = status.playlistRevision;
    out["sessionId"]        = status.sessionId;
    out["apiVersion"]       = 1;
    return out;
}

nlohmann::json toJson(const TrackSummary& track) {
    nlohmann::json out;
    out["id"]         = track.id;
    out["url"]        = track.url;
    out["title"]      = track.title;
    out["artist"]     = track.artist;
    out["album"]      = track.album;
    out["duration"]   = track.duration;
    out["track"]      = track.track;
    out["disc"]       = track.disc;
    out["playCount"]  = track.playCount;
    out["queued"]     = track.queued;
    out["stopAfter"]  = track.stopAfter;
    out["error"]      = track.error;
    out["hasArtwork"] = track.hasArtwork;
    return out;
}

nlohmann::json toJson(const TrackDetail& track) {
    nlohmann::json out = toJson(track.summary);
    out["albumArtist"]  = track.albumArtist;
    out["genre"]        = track.genre;
    out["composer"]     = track.composer;
    out["date"]         = track.date;
    out["comment"]      = track.comment;
    out["lyrics"]       = track.lyrics;
    out["errorMessage"] = track.errorMessage;

    nlohmann::json properties;
    properties["sampleRate"]    = track.sampleRate;
    properties["channels"]      = track.channels;
    properties["bitsPerSample"] = track.bitsPerSample;
    properties["bitrateKbps"]   = track.bitrateKbps;
    properties["seekable"]      = track.seekable;
    properties["lossless"]      = track.lossless;
    properties["cuesheet"]      = track.cuesheet;
    out["properties"]           = std::move(properties);

    // An object rather than an array of pairs: the keys are unique by the time
    // they get here, and a client reading a tag wants to name it.
    nlohmann::json metadata = nlohmann::json::object();
    for (const auto& [key, value] : track.metadata) {
        metadata[key] = value;
    }
    out["metadata"] = std::move(metadata);
    return out;
}

nlohmann::json toJson(const SettingInfo& setting) {
    nlohmann::json out;
    out["key"]         = setting.key;
    out["type"]        = setting.type;
    out["value"]       = setting.value;
    out["default"]     = setting.defaultValue;
    out["writable"]    = setting.writable;
    out["appliesFrom"] = setting.appliesFrom;
    return out;
}

nlohmann::json toJson(const EqualizerState& equalizer) {
    nlohmann::json out;
    out["enabled"]    = equalizer.enabled;
    out["preamp"]     = equalizer.preamp;
    out["trackGenre"] = equalizer.trackGenre;

    nlohmann::json bands = nlohmann::json::array();
    for (const auto& [hz, gain] : equalizer.bands) {
        nlohmann::json band;
        band["hz"]   = hz;
        band["gain"] = gain;
        bands.push_back(std::move(band));
    }
    out["bands"] = std::move(bands);
    // The equaliser is the one DSP control that moves what is already playing;
    // saying so beside every other appliesFrom is what keeps the pair honest.
    out["appliesFrom"] = "immediately";
    return out;
}

nlohmann::json toJson(const JobStatus& job) {
    nlohmann::json out;
    out["id"]    = job.id;
    out["state"] = job.state;

    nlohmann::json progress;
    progress["done"]  = job.done;
    progress["total"] = job.total;
    out["progress"]   = std::move(progress);

    out["added"] = job.added;
    out["error"] = job.error.empty() ? nlohmann::json{} : nlohmann::json(job.error);
    return out;
}

std::optional<std::vector<TrackId>> readIds(const nlohmann::json& body,
                                            std::string_view      field) {
    const auto found = body.find(field);
    if (found == body.end() || !found->is_array()) {
        return std::nullopt;
    }
    std::vector<TrackId> ids;
    ids.reserve(found->size());
    for (const nlohmann::json& element : *found) {
        if (!element.is_number_unsigned()) {
            return std::nullopt;
        }
        ids.push_back(element.get<TrackId>());
    }
    return ids;
}

std::optional<double> readNumber(const nlohmann::json& body, std::string_view field) {
    const auto found = body.find(field);
    // is_number() is true for a bool in some JSON libraries and not in this one;
    // the guard is here anyway because the answer matters and is easy to assume.
    if (found == body.end() || !found->is_number() || found->is_boolean()) {
        return std::nullopt;
    }
    return found->get<double>();
}

std::optional<bool> readBool(const nlohmann::json& body, std::string_view field) {
    const auto found = body.find(field);
    if (found == body.end() || !found->is_boolean()) {
        return std::nullopt;
    }
    return found->get<bool>();
}

std::optional<std::string> readString(const nlohmann::json& body,
                                      std::string_view      field) {
    const auto found = body.find(field);
    if (found == body.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

}  // namespace xpcog::remote
