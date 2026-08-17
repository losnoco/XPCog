// A minimal Apple XML property list reader and writer.
//
// Cog's "XML" playlist is not an ad-hoc format: PlaylistLoader.m:243 serialises
// an NSDictionary with NSPropertyListXMLFormat_v1_0, and XmlContainer.m reads it
// back with NSPropertyListSerialization. Reading and writing Cog's own playlists
// therefore means handling plists, on platforms that have never heard of them.
//
// Scope is exactly what Cog's playlists contain: dict, array, string, integer,
// real, true/false, data. `<date>` is accepted and kept as a string rather than
// parsed, because nothing in a playlist reads one.
//
// This is not a general plist implementation -- no binary format, no entity
// declarations, no external DTD fetching (which is a fine way to turn opening a
// playlist into a network request).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xpcog::plist {

struct Value {
    enum class Type { Null, Bool, Integer, Real, String, Data, Array, Dict };

    Type                   type    = Type::Null;
    bool                   boolean = false;
    std::int64_t           integer = 0;
    double                 real    = 0.0;
    std::string            string;
    std::vector<std::byte> data;
    // std::vector over an incomplete type is well-defined since C++17, which is
    // what lets these nest without a layer of indirection.
    std::vector<Value>                         array;
    std::vector<std::pair<std::string, Value>> dict;

    [[nodiscard]] static Value ofString(std::string text);
    [[nodiscard]] static Value ofInteger(std::int64_t number);
    [[nodiscard]] static Value ofReal(double number);
    [[nodiscard]] static Value ofBool(bool flag);
    [[nodiscard]] static Value ofData(std::vector<std::byte> bytes);
    [[nodiscard]] static Value ofArray(std::vector<Value> items);
    [[nodiscard]] static Value ofDict(std::vector<std::pair<std::string, Value>> items);

    /// Value for `key`, or nullptr. Only meaningful on a dict.
    [[nodiscard]] const Value* find(std::string_view key) const;

    /// Convenience readers that coerce the way a playlist reader wants: a
    /// missing or wrongly-typed value reads as the default rather than throwing,
    /// because a playlist written by another tool is not a contract.
    [[nodiscard]] std::string  stringValue(std::string_view key,
                                           std::string_view fallback = {}) const;
    [[nodiscard]] std::int64_t integerValue(std::string_view key,
                                            std::int64_t     fallback = 0) const;
    [[nodiscard]] double       realValue(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] bool         boolValue(std::string_view key, bool fallback = false) const;
};

/// Parses an XML property list. Returns nullopt on malformed input.
[[nodiscard]] std::optional<Value> parse(std::string_view text);

/// Serialises in the same shape Foundation writes, so a Cog that reads the file
/// back sees what it expects.
[[nodiscard]] std::string write(const Value& root);

[[nodiscard]] std::string            base64Encode(const std::vector<std::byte>& data);
[[nodiscard]] std::vector<std::byte> base64Decode(std::string_view text);

/// XML text helpers, shared with the XSPF reader and writer so escaping is
/// defined in one place.
[[nodiscard]] std::string xmlEscape(std::string_view text);
[[nodiscard]] std::string xmlDecodeEntities(std::string_view raw);

}  // namespace xpcog::plist
