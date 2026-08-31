// The player's structs as JSON, and JSON back into the few things a request
// carries.
//
// Here rather than beside the structs because nlohmann is linked PRIVATE to
// xpcog-core: PlayerControl.hpp names std:: types precisely so the parser stays
// out of core's public surface, and this is the file that is allowed to know
// about both.
//
// Serialisation happens on the HTTP thread, not under the gate. The gate returns
// plain structs and this turns them into bytes afterwards, which is the
// difference between holding the interface thread for a hop and holding it for a
// hop plus however long it takes to format ten thousand playlist rows.

#pragma once

#include "xpcog/core/remote/PlayerControl.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace xpcog::remote {

[[nodiscard]] nlohmann::json toJson(const Status& status);
[[nodiscard]] nlohmann::json toJson(const TrackSummary& track);
[[nodiscard]] nlohmann::json toJson(const TrackDetail& track);
[[nodiscard]] nlohmann::json toJson(const SettingInfo& setting);
[[nodiscard]] nlohmann::json toJson(const EqualizerState& equalizer);
[[nodiscard]] nlohmann::json toJson(const JobStatus& job);

/// Reads a list of track ids out of `field`.
///
/// nullopt when the field is missing or is not an array of numbers, which the
/// caller turns into a 400 naming the field -- an id list that silently became
/// empty would be a delete that quietly did nothing.
[[nodiscard]] std::optional<std::vector<TrackId>> readIds(const nlohmann::json& body,
                                                          std::string_view field);

/// Reads a number, refusing a bool -- JSON says `true` is not 1, and a client
/// that sent one has a bug worth reporting rather than rounding off.
[[nodiscard]] std::optional<double> readNumber(const nlohmann::json& body,
                                               std::string_view field);

[[nodiscard]] std::optional<bool> readBool(const nlohmann::json& body,
                                           std::string_view field);

[[nodiscard]] std::optional<std::string> readString(const nlohmann::json& body,
                                                    std::string_view field);

}  // namespace xpcog::remote
