// Every playlist the listener has, and the two that matter at any moment: the
// one on screen and the one playing. Port of what Cog added in 0295388af and
// 14ed421cd (CogPlaylist's Playlist entity and PlaybackOrder).
//
// Playlist keeps its traversal rules and knows nothing of the others, with two
// exceptions it shares through Playlist::Shared: track ids, because the store
// keeps every entry in one table under one key, and the queue, because Cog's is
// one for every playlist. A queued track from another playlist therefore comes
// up in the middle of this one, and when the queue empties playback goes on
// after the last entry it played, in that entry's playlist. That hand-over is
// what nextForPlayback() and next() here are for, and why the playback
// controller asks this class rather than a Playlist.
//
// `shown` and `playing` are independent. Looking at another playlist never
// changes what plays next, which is the rule Cog's 14ed421cd exists to state.
//
// --- Threads ---------------------------------------------------------------
//
// The feeder thread asks nextForPlayback() while the interface adds, moves and
// removes tabs, which unlike a playlist edit is not something it stops playback
// for. So the list of playlists is under a lock, and nothing published here is
// published from the feeder: `playing` moves in noteAudible(), which is on the
// interface's thread, and until then only the read-ahead cursor has crossed.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {

class PlaylistSet {
public:
    /// Stable across sessions and across reordering. Never reused while the
    /// set is open, so an id held by a scan still running means what it did.
    using Id = std::int64_t;

    /// The playlist a store from before there were several moves into.
    static constexpr Id kFirstId = 1;

    struct Item {
        Id                        id = 0;
        /// Empty means "not named", and the interface supplies a word. The
        /// first playlist of an upgraded store is that: core has no catalogue.
        std::string               name;
        std::unique_ptr<Playlist> playlist;
        Subscription              watch;
    };

    struct Change {
        enum class Kind {
            Added,    ///< `id` is new
            Removed,  ///< `id` is gone
            Moved,    ///< the order changed
            Renamed,  ///< `id` has a new name
            Shown,    ///< `id` is on screen now
            Playing,  ///< `id` is the one playing now
        };
        Kind kind = Kind::Added;
        Id   id   = 0;
    };

    /// Starts with one empty playlist. There is never fewer.
    PlaylistSet();

    PlaylistSet(const PlaylistSet&)            = delete;
    PlaylistSet& operator=(const PlaylistSet&) = delete;

    /// Told about the set changing shape. For what is inside a playlist, see
    /// observeContent().
    [[nodiscard]] Subscription observe(std::function<void(const Change&)> observer) {
        return changed_.connect(std::move(observer));
    }

    /// Told about a change in any playlist, with which one it was.
    [[nodiscard]] Subscription observeContent(
        std::function<void(Id, const Playlist::Change&)> observer) {
        return contentChanged_.connect(std::move(observer));
    }

    // --- the playlists ------------------------------------------------------

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const Item& at(std::size_t index) const { return *items_[index]; }
    [[nodiscard]] std::optional<std::size_t> indexOf(Id id) const;
    [[nodiscard]] Playlist*       playlist(Id id);
    [[nodiscard]] const Playlist* playlist(Id id) const;

    /// A new empty playlist at the end, or before `position`.
    Id add(std::string name, std::optional<std::size_t> position = std::nullopt);

    /// Refused for the last one. The one shown gives way to its neighbour, and
    /// so does the one playing; stopping playback first is the caller's, since
    /// this class cannot reach the engine.
    bool remove(Id id);

    bool rename(Id id, std::string name);
    bool move(Id id, std::size_t position);

    // --- the two that matter ------------------------------------------------

    [[nodiscard]] Id        shownId() const noexcept { return shown_; }
    [[nodiscard]] Id        playingId() const noexcept { return playing_; }
    [[nodiscard]] Playlist& shown() { return *playlist(shown_); }
    [[nodiscard]] Playlist& playing() { return *playlist(playing_); }
    [[nodiscard]] const Playlist& shown() const { return *playlist(shown_); }
    [[nodiscard]] const Playlist& playing() const { return *playlist(playing_); }

    void show(Id id);

    /// Makes `id` the one playing. What was current in the one before stops
    /// being current, so at most one playlist ever has a playing row.
    void setPlaying(Id id);

    // --- entries, wherever they are ------------------------------------------

    [[nodiscard]] const PlaylistEntry* find(TrackId id) const;

    /// Which playlist holds `id`, or null.
    [[nodiscard]] Playlist*          ownerOf(TrackId id);
    [[nodiscard]] std::optional<Id>  ownerIdOf(TrackId id) const;

    /// The entry with this URL: in the playing playlist if it is there, because
    /// the engine reports URLs and the same file can be in two playlists.
    [[nodiscard]] std::optional<TrackId> findByUrl(const std::string& url) const;

    void update(TrackId id, const std::function<void(PlaylistEntry&)>& mutate);

    // --- playback across playlists -------------------------------------------

    /// Playlist::nextForPlayback(), following the queue into another playlist.
    /// The cursor crosses here, a buffer ahead of the audio, so the entry after
    /// this one is measured from where it now is. `playing` waits for
    /// noteAudible(): Next pressed in between means the track being heard.
    [[nodiscard]] std::optional<TrackId> nextForPlayback();

    /// Playlist::peekNextForPlayback() on the playing one. Consumes nothing.
    [[nodiscard]] std::optional<TrackId> peekNextForPlayback() const;

    /// Playlist::next(), the same way.
    bool next();

    /// Moves `id` to current in its own playlist and makes that the playing one.
    void setCurrent(TrackId id);

    /// `id` reached the speaker. Only now does its playlist become the playing
    /// one, and the one the audio came from let go of its playing row.
    void noteAudible(TrackId id);

    // --- every playlist at once -----------------------------------------------

    void setRepeat(RepeatMode mode);
    void setShuffle(ShuffleMode mode);
    void setStopAfterCurrent(bool stop);

    /// One playlist as the store holds it.
    struct Stored {
        Id                 id = 0;
        std::string        name;
        Playlist::Snapshot state;
    };

    /// Replaces everything. `queue` is the shared queue in order, across all of
    /// them. Empty `playlists` leaves one empty playlist rather than none.
    void restore(std::vector<Stored> playlists, std::vector<TrackId> queue);

private:
    [[nodiscard]] Item* item(Id id);
    [[nodiscard]] const Item* item(Id id) const;
    [[nodiscard]] Item& append(Id id, std::string name, std::optional<std::size_t> position);
    void syncQueue();
    void publish(Change change) const { changed_.publish(change); }

    std::shared_ptr<Playlist::Shared> shared_;
    std::vector<std::unique_ptr<Item>> items_;
    Id nextId_  = kFirstId;
    Id shown_   = kFirstId;
    Id playing_ = kFirstId;
    /// Whose read-ahead cursor is live. `playing_` but for the moments between
    /// the queue crossing into another playlist and that audio being heard.
    Id cursorOwner_ = kFirstId;

    mutable std::recursive_mutex mutex_;

    RepeatMode  repeat_     = RepeatMode::All;
    ShuffleMode shuffle_    = ShuffleMode::Off;
    bool        stopAfter_  = false;

    Signal<Change>                 changed_;
    Signal<Id, Playlist::Change>   contentChanged_;
};

}  // namespace xpcog
