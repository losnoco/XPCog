// Playback order, shuffle, repeat and the queue. Port of the model half of
// Cog's Playlist/PlaylistController.m (2,425 lines).
//
// Cog's class is an NSArrayController subclass, so it is simultaneously the
// model, the controller and the table's view state -- and its notion of "the
// list" is `arrangedObjects`, the *sorted* order. That is why sorting a Cog
// playlist by album silently changes what plays next, a long-standing source of
// confusion.
//
// Here the split is explicit: this class owns the canonical order and knows
// nothing about columns or sorting, and PlaylistProxyModel in the app layer is
// display-only. **Sort order therefore never affects playback order.** That is a
// deliberate behaviour change from Cog.
//
// Repeat and shuffle are plain state, not settings lookups. Cog reads
// NSUserDefaults on every call; keeping them here means the traversal logic is
// testable without a settings store, and the app syncs the two in one line.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace xpcog {

/// Values match Cog's RepeatMode (Playlist/PlaylistControllerEnums.h) because
/// they are what its preferences plist stores.
enum class RepeatMode : int {
    None  = 0,
    One   = 1,
    Album = 2,
    All   = 3,
};

/// Values match Cog's ShuffleMode.
enum class ShuffleMode : int {
    Off    = 0,
    Albums = 1,
    All    = 2,
};

class Playlist {
public:
    struct Change {
        enum class Kind {
            Reset,     ///< everything changed; reload
            Inserted,  ///< `count` rows at `index`
            Removed,   ///< `count` rows that were at `index`
            Moved,     ///< `count` rows from `index` to `destination`
            Updated,   ///< `count` rows at `index` changed in place
            Current,   ///< the playing entry changed
            Queue,     ///< the queue changed
            Order,     ///< shuffle or repeat mode changed
        };

        Kind        kind        = Kind::Reset;
        std::size_t index       = 0;
        std::size_t count       = 0;
        std::size_t destination = 0;  ///< Moved only
    };

    Playlist();

    /// Fixes the shuffle order for a test. Without this the generator is seeded
    /// from std::random_device, as Cog seeds srandom() from time().
    void seedShuffle(std::uint64_t seed) noexcept { rng_.seed(seed); }

    [[nodiscard]] Subscription observe(std::function<void(const Change&)> observer) {
        return changed_.connect(std::move(observer));
    }

    // --- content --------------------------------------------------------

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return entries_.empty(); }

    [[nodiscard]] const PlaylistEntry& at(std::size_t index) const { return entries_[index]; }
    [[nodiscard]] const std::vector<PlaylistEntry>& entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] const PlaylistEntry*       find(TrackId id) const;
    [[nodiscard]] std::optional<std::size_t> indexOf(TrackId id) const;

    /// Appends and assigns an id. The id in `entry` is ignored.
    TrackId add(PlaylistEntry entry);

    /// Inserts at `index`, assigning ids. Returns the assigned ids in order.
    std::vector<TrackId> insert(std::size_t index, std::vector<PlaylistEntry> entries);

    void removeAt(std::size_t index, std::size_t count = 1);
    void remove(const std::vector<TrackId>& ids);
    void clear();

    /// Moves `count` rows starting at `index` so they begin at `destination`,
    /// where `destination` is expressed in the list as it is *before* the move.
    void move(std::size_t index, std::size_t count, std::size_t destination);

    /// Mutates one entry in place and notifies. The id and url are restored
    /// afterwards, so a mutator cannot break the id index.
    void update(TrackId id, const std::function<void(PlaylistEntry&)>& mutate);

    /// Reorders the playlist itself into a random permutation. Cog's
    /// -randomizeList: this is a real, undoable edit, unlike shuffle.
    void randomize();

    // --- modes ----------------------------------------------------------

    [[nodiscard]] RepeatMode  repeat() const noexcept { return repeat_; }
    [[nodiscard]] ShuffleMode shuffle() const noexcept { return shuffle_; }

    void setRepeat(RepeatMode mode);
    /// Switching to a shuffled mode rebuilds the order with the current entry at
    /// its head, so shuffling mid-track does not restart the track.
    void setShuffle(ShuffleMode mode);

    /// Cog's `alwaysStopAfterCurrent`: playback stops at the end of every track.
    void setStopAfterCurrent(bool stop) noexcept { stopAfterCurrent_ = stop; }
    [[nodiscard]] bool stopAfterCurrent() const noexcept { return stopAfterCurrent_; }

    // --- current --------------------------------------------------------

    [[nodiscard]] std::optional<TrackId> current() const noexcept { return current_; }
    [[nodiscard]] const PlaylistEntry*   currentEntry() const;
    void setCurrent(std::optional<TrackId> id);

    // --- queue ----------------------------------------------------------

    [[nodiscard]] const std::vector<TrackId>& queue() const noexcept { return queue_; }
    void enqueue(TrackId id);
    void dequeue(TrackId id);
    void clearQueue();

    // --- traversal ------------------------------------------------------

    /// What to play when the current track ends. Honours repeat-one and
    /// `stopAfterCurrent`. Port of -getNextEntry: with ignoreRepeatOne:NO.
    ///
    /// **This consumes state**: it pops the queue and extends the shuffle order.
    /// Call it once per track transition and remember the answer -- which is what
    /// gapless preloading needs anyway.
    [[nodiscard]] std::optional<TrackId> nextForPlayback();

    /// The user pressed Next. Repeat-one is ignored -- otherwise the button would
    /// do nothing -- and the current entry is updated. Cog's -next.
    bool next();

    /// The user pressed Previous. Cog's -prev.
    bool previous();

    /// What Previous would select, without selecting it.
    [[nodiscard]] std::optional<TrackId> peekPrevious();

    // --- persistence ----------------------------------------------------

    /// Everything a Library needs to store, and everything restoring needs to
    /// bring back. Having one seam means persistence cannot quietly forget a
    /// field: adding state to Playlist that is not in the snapshot is visible
    /// here rather than as a setting that silently fails to survive a restart.
    struct Snapshot {
        std::vector<PlaylistEntry> entries;  ///< ids are meaningful and preserved
        std::vector<TrackId>       queue;
        std::vector<TrackId>       shuffleOrder;
        std::optional<TrackId>     current;
        RepeatMode                 repeat  = RepeatMode::All;
        ShuffleMode                shuffle = ShuffleMode::Off;
    };

    [[nodiscard]] Snapshot snapshot() const;

    /// Replaces the entire contents. Ids in `entries` are kept as given, so a
    /// reload preserves whatever else referenced them.
    void restore(Snapshot state);

private:
    /// Where playback begins when nothing is current: the head of the shuffle
    /// order when shuffling, otherwise the top of the list.
    [[nodiscard]] std::optional<TrackId> firstTrack();
    [[nodiscard]] std::optional<TrackId> lastTrack();

    [[nodiscard]] std::optional<TrackId> nextEntry(TrackId from, bool ignoreRepeatOne);
    [[nodiscard]] std::optional<TrackId> previousEntry(TrackId from, bool ignoreRepeatOne);

    /// shuffleList_[i], extending the list in either direction when repeating
    /// all. Port of -shuffledEntryAtIndex:.
    [[nodiscard]] std::optional<TrackId> shuffledEntryAt(std::int64_t i);

    [[nodiscard]] std::vector<TrackId> shuffledOrder();
    [[nodiscard]] std::vector<TrackId> albumShuffledOrder();
    void                               addShuffledListToFront();
    void                               addShuffledListToBack();
    void                               resetShuffleList();

    /// Indices of every entry whose album matches, case-insensitively.
    /// Port of -filterPlaylistOnAlbum:.
    [[nodiscard]] std::vector<std::size_t> indicesForAlbum(std::string_view album) const;

    void reindex();
    void renumberQueue();
    void notify(Change change) const { changed_.emit(change); }

    std::vector<PlaylistEntry>                 entries_;
    std::unordered_map<TrackId, std::size_t>   index_;
    TrackId                                    nextId_ = 1;

    std::vector<TrackId> shuffleList_;
    std::vector<TrackId> queue_;

    std::optional<TrackId> current_;

    /// Set when the current entry is removed, so the next step resumes where it
    /// was rather than at the top of the playlist. Replaces Cog's
    /// `nextEntryAfterDeleted` plus its negative-index encoding.
    ///
    /// The flag is separate from the id because "the playing entry was the last
    /// one" and "there is no playing entry" are different questions: the first
    /// ends playback, the second starts it.
    bool                   currentRemoved_ = false;
    std::optional<TrackId> resumeAt_;

    RepeatMode  repeat_  = RepeatMode::All;
    ShuffleMode shuffle_ = ShuffleMode::Off;
    bool        stopAfterCurrent_ = false;

    std::mt19937_64 rng_;

    Signal<Change> changed_;
};

}  // namespace xpcog
