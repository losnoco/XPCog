// Replaces the metadata NSDictionary passed across Cog's plugin boundary.
//
// Value shape verified against Cog Playlist/PlaylistEntry.m:593-682 and
// Plugins/Flac/FlacDecoder.m:185-193: every value is either a list of strings
// (tags legitimately repeat -- multiple ARTIST lines, several GENREs) or raw
// bytes (album art). Keys are lowercased; insertion order is preserved because
// tag display order is user-visible.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xpcog {

using MetaValue = std::variant<std::vector<std::string>, std::vector<std::byte>>;

class MetadataMap {
public:
    struct Entry {
        std::string key;
        MetaValue   value;

        /// Needed for MetadataMap's own comparison below, which is only defined
        /// when the element type has one -- so without this the map compares
        /// only in code that never instantiates it.
        [[nodiscard]] friend bool operator==(const Entry&, const Entry&) = default;
    };

    /// Replaces any existing value for `key`.
    void set(std::string_view key, std::string value);
    void set(std::string_view key, std::vector<std::string> values);
    void setBytes(std::string_view key, std::vector<std::byte> bytes);

    /// Appends to a multi-value tag, creating it if absent. Cog's setDictionary().
    void add(std::string_view key, std::string value);

    [[nodiscard]] const MetaValue* find(std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }

    /// First string value, or empty. Bytes values yield empty.
    [[nodiscard]] std::string_view first(std::string_view key) const;

    /// All string values joined. Equivalent to Cog's -readAllValuesAsString:.
    [[nodiscard]] std::string joined(std::string_view key,
                                     std::string_view separator = ", ") const;

    [[nodiscard]] const std::vector<std::byte>* bytes(std::string_view key) const;

    /// Copies entries from `other`, overwriting on key collision.
    /// Replaces Cog's NSDictionary+Merge.
    void mergeFrom(const MetadataMap& other);

    void remove(std::string_view key);
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] bool        empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] auto end() const noexcept { return entries_.end(); }

    [[nodiscard]] friend bool operator==(const MetadataMap&,
                                         const MetadataMap&) = default;

    /// Lowercases; also maps '.' to U+2024 ONE DOT LEADER exactly as Cog does
    /// (FlacDecoder.m:186), because '.' is a key-path separator in Cocoa bindings.
    /// Kept for tag-name compatibility with libraries written by Cog.
    [[nodiscard]] static std::string normalizeKey(std::string_view key);

private:
    [[nodiscard]] Entry* findEntry(std::string_view normalizedKey);

    std::vector<Entry> entries_;
};

}  // namespace xpcog
