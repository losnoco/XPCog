#include "xpcog/core/MetadataMap.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace xpcog {
namespace {

constexpr std::string_view kOneDotLeader = "․";  // U+2024, as Cog uses

}  // namespace

std::string MetadataMap::normalizeKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());

    for (const char c : key) {
        if (c == '.') {
            out.append(kOneDotLeader);
        } else {
            out.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool MetadataMap::sameContentAs(const MetadataMap& other) const {
    // Keys are unique within a map, so matching sizes plus every key found with
    // an equal value is enough; no need to check the other direction.
    if (entries_.size() != other.entries_.size()) {
        return false;
    }
    for (const Entry& mine : entries_) {
        const auto theirs =
            std::find_if(other.entries_.begin(), other.entries_.end(),
                         [&](const Entry& entry) { return entry.key == mine.key; });
        if (theirs == other.entries_.end() || !(theirs->value == mine.value)) {
            return false;
        }
    }
    return true;
}

MetadataMap::Entry* MetadataMap::findEntry(std::string_view normalizedKey) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.key == normalizedKey; });
    return it == entries_.end() ? nullptr : &*it;
}

void MetadataMap::set(std::string_view key, std::string value) {
    set(key, std::vector<std::string>{std::move(value)});
}

void MetadataMap::set(std::string_view key, std::vector<std::string> values) {
    const std::string normalized = normalizeKey(key);
    if (Entry* existing = findEntry(normalized)) {
        existing->value = std::move(values);
        return;
    }
    entries_.push_back(Entry{normalized, std::move(values)});
}

void MetadataMap::setBytes(std::string_view key, std::vector<std::byte> bytes) {
    const std::string normalized = normalizeKey(key);
    if (Entry* existing = findEntry(normalized)) {
        existing->value = std::move(bytes);
        return;
    }
    entries_.push_back(Entry{normalized, std::move(bytes)});
}

void MetadataMap::add(std::string_view key, std::string value) {
    const std::string normalized = normalizeKey(key);
    if (Entry* existing = findEntry(normalized)) {
        if (auto* list = std::get_if<std::vector<std::string>>(&existing->value)) {
            list->push_back(std::move(value));
        } else {
            // A bytes value under this key; a repeated string tag replaces it.
            existing->value = std::vector<std::string>{std::move(value)};
        }
        return;
    }
    entries_.push_back(Entry{normalized, std::vector<std::string>{std::move(value)}});
}

const MetaValue* MetadataMap::find(std::string_view key) const {
    const std::string normalized = normalizeKey(key);
    const auto        it =
        std::find_if(entries_.begin(), entries_.end(),
                     [&](const Entry& e) { return e.key == normalized; });
    return it == entries_.end() ? nullptr : &it->value;
}

std::string_view MetadataMap::first(std::string_view key) const {
    if (const MetaValue* value = find(key)) {
        if (const auto* list = std::get_if<std::vector<std::string>>(value)) {
            if (!list->empty()) {
                return list->front();
            }
        }
    }
    return {};
}

std::string MetadataMap::joined(std::string_view key,
                                std::string_view separator) const {
    const MetaValue* value = find(key);
    if (value == nullptr) {
        return {};
    }
    const auto* list = std::get_if<std::vector<std::string>>(value);
    if (list == nullptr) {
        return {};
    }

    std::string out;
    for (std::size_t i = 0; i < list->size(); ++i) {
        if (i != 0) {
            out.append(separator);
        }
        out.append((*list)[i]);
    }
    return out;
}

const std::vector<std::byte>* MetadataMap::bytes(std::string_view key) const {
    if (const MetaValue* value = find(key)) {
        return std::get_if<std::vector<std::byte>>(value);
    }
    return nullptr;
}

void MetadataMap::mergeFrom(const MetadataMap& other) {
    for (const Entry& entry : other.entries_) {
        // Keys in `other` are already normalized, so bypass normalizeKey().
        if (Entry* existing = findEntry(entry.key)) {
            existing->value = entry.value;
        } else {
            entries_.push_back(entry);
        }
    }
}

void MetadataMap::remove(std::string_view key) {
    const std::string normalized = normalizeKey(key);
    std::erase_if(entries_, [&](const Entry& e) { return e.key == normalized; });
}

}  // namespace xpcog
