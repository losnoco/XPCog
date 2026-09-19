// The library's database as the lyrics cache.
//
// Two forwarding calls, kept out of Library itself so that Library does not
// inherit an interface for the sake of one client, and out of LyricsLookup so
// that the lookup never names the database. The application builds one over
// its Library and hands it to the lookup; both are used on the interface
// thread, which is the only thread the Library is used from.

#pragma once

#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/lyrics/LyricsStore.hpp"

namespace xpcog {

class LibraryLyricsStore final : public ILyricsStore {
public:
    /// `library` is borrowed and must outlive the store.
    explicit LibraryLyricsStore(Library& library) : library_(library) {}

    [[nodiscard]] std::optional<StoredLyrics> load(std::string_view key) const override {
        return library_.cachedLyrics(key);
    }

    bool store(std::string_view key, const StoredLyrics& record) override {
        return library_.storeCachedLyrics(key, record);
    }

private:
    Library& library_;
};

}  // namespace xpcog
