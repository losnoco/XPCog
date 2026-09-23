// Where fetched lyrics are kept between sessions.
//
// The seam between LyricsLookup and whatever persists its answers, so the
// lookup can be tested against a map and the application can hand it the
// library's database. Two calls, both on the interface thread -- the same
// thread the Library is used from everywhere else -- so an implementation
// needs no lock of its own.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xpcog {

/// One remembered answer. A failure is never stored: it says something about
/// the network at the time, not about the track.
struct StoredLyrics {
    /// The service had an entry for the track. False is "not found", and is
    /// kept too -- for a while -- because it is the commonest answer and the
    /// one a listener would otherwise trigger again on every selection.
    bool known = false;
    /// The entry says the track has no words.
    bool instrumental = false;
    /// Plain and LRC, either or both possibly empty. The pane shows the LRC
    /// when there is one and follows it through playback.
    std::string plain;
    std::string synced;
    /// Unix seconds when the answer arrived. What decides whether a "not
    /// found" is old enough to ask again.
    std::int64_t fetchedAt = 0;
};

class ILyricsStore {
public:
    virtual ~ILyricsStore() = default;

    /// The answer stored under `key`, if any.
    [[nodiscard]] virtual std::optional<StoredLyrics> load(std::string_view key) const = 0;

    /// Stores `record` under `key`, replacing what was there. False when the
    /// store refused; the lookup carries on with its in-memory copy.
    virtual bool store(std::string_view key, const StoredLyrics& record) = 0;
};

}  // namespace xpcog
