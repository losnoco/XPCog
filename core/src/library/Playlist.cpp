#include "xpcog/core/library/Playlist.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <unordered_set>

namespace xpcog {
namespace {

[[nodiscard]] bool sameAlbum(std::string_view a, std::string_view b) {
    // Cog uses -caseInsensitiveCompare:, which is Unicode-aware. ASCII folding
    // covers the case that actually occurs (ID3 vs Vorbis capitalisation); full
    // case folding arrives with the collator in the app layer.
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Playlist::Playlist() {
    std::random_device device;
    rng_.seed((static_cast<std::uint64_t>(device()) << 32) ^ device());
}

// --- lookup -------------------------------------------------------------

std::optional<std::size_t> Playlist::indexOf(TrackId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return std::nullopt;
    }
    return it->second;
}

const PlaylistEntry* Playlist::find(TrackId id) const {
    const auto position = indexOf(id);
    return position ? &entries_[*position] : nullptr;
}

const PlaylistEntry* Playlist::currentEntry() const {
    return current_ ? find(*current_) : nullptr;
}

void Playlist::reindex() {
    index_.clear();
    index_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        index_[entries_[i].id] = i;
    }
}

// --- content ------------------------------------------------------------

TrackId Playlist::add(PlaylistEntry entry) {
    return insert(entries_.size(), {std::move(entry)}).front();
}

std::vector<TrackId> Playlist::insert(std::size_t index,
                                      std::vector<PlaylistEntry> entries) {
    index = std::min(index, entries_.size());

    std::vector<TrackId> ids;
    ids.reserve(entries.size());
    for (auto& entry : entries) {
        entry.id = nextId_++;
        ids.push_back(entry.id);
    }

    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(index),
                    std::make_move_iterator(entries.begin()),
                    std::make_move_iterator(entries.end()));
    reindex();

    // New entries are not spliced into the existing shuffle order: doing so would
    // mean deciding where in an already-drawn permutation they belong, and any
    // answer is arbitrary. They join at the next permutation, which is also what
    // Cog does, since its shuffle list is only extended, never patched.
    notify({Change::Kind::Inserted, index, ids.size(), 0});
    return ids;
}

void Playlist::reinsert(std::size_t index, std::vector<PlaylistEntry> entries) {
    if (entries.empty()) {
        return;
    }
    index = std::min(index, entries_.size());

    const std::size_t count = entries.size();
    for (PlaylistEntry& entry : entries) {
        // Keep the counter ahead of anything restored, or a later add() would
        // hand out an id that is already in the list.
        nextId_ = std::max(nextId_, entry.id + 1);

        // Neither the queue nor the shuffle order is restored here, so an entry
        // must not come back claiming a place in either. Leaving the stale
        // values on would make queued() true for an entry that is not in
        // queue_, and enqueue() refuses to re-add something already queued --
        // so restoring the queue afterwards would silently do nothing.
        entry.queuePosition = -1;
        entry.shuffleIndex  = -1;
    }

    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(index),
                    std::make_move_iterator(entries.begin()),
                    std::make_move_iterator(entries.end()));
    reindex();

    notify({Change::Kind::Inserted, index, count, 0});
}

bool Playlist::reorder(const std::vector<TrackId>& order) {
    if (order.size() != entries_.size()) {
        return false;
    }

    std::vector<PlaylistEntry> rearranged;
    rearranged.reserve(order.size());
    for (const TrackId id : order) {
        const auto position = indexOf(id);
        if (!position) {
            return false;  // not a permutation; leave the playlist alone
        }
        rearranged.push_back(entries_[*position]);
    }
    // An id repeated in `order` would pass the check above while dropping a
    // different entry, so verify the result covers everything exactly once.
    if (std::unordered_set<TrackId>(order.begin(), order.end()).size() != order.size()) {
        return false;
    }

    entries_ = std::move(rearranged);
    reindex();
    notify({Change::Kind::Reset, 0, 0, 0});
    return true;
}

void Playlist::removeAt(std::size_t index, std::size_t count) {
    if (index >= entries_.size() || count == 0) {
        return;
    }
    count = std::min(count, entries_.size() - index);

    std::unordered_set<TrackId> removed;
    removed.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        removed.insert(entries_[index + i].id);
    }

    // Losing the playing entry has to leave a place to resume from, or the next
    // track jumps to the top of the playlist. Cog carries a `nextEntryAfterDeleted`
    // pointer plus a `deLeted` flag on the entry for this; recording the id of
    // whatever survives at the same position does the same job without either.
    if (current_ && removed.contains(*current_)) {
        const std::size_t after = index + count;
        currentRemoved_ = true;
        resumeAt_ =
            (after < entries_.size()) ? std::optional{entries_[after].id} : std::nullopt;
    }

    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index),
                   entries_.begin() + static_cast<std::ptrdiff_t>(index + count));
    reindex();

    const auto drop = [&removed](std::vector<TrackId>& list) {
        std::erase_if(list, [&removed](TrackId id) { return removed.contains(id); });
    };
    drop(shuffleList_);

    const std::size_t queueBefore = queue_.size();
    drop(queue_);
    if (queue_.size() != queueBefore) {
        renumberQueue();
    }

    notify({Change::Kind::Removed, index, count, 0});
    if (queue_.size() != queueBefore) {
        notify({Change::Kind::Queue, 0, 0, 0});
    }
}

void Playlist::remove(const std::vector<TrackId>& ids) {
    // Descending, so each erase leaves the remaining indices valid.
    std::vector<std::size_t> positions;
    positions.reserve(ids.size());
    for (const TrackId id : ids) {
        if (const auto position = indexOf(id)) {
            positions.push_back(*position);
        }
    }
    std::sort(positions.begin(), positions.end(), std::greater<>{});
    for (const std::size_t position : positions) {
        removeAt(position, 1);
    }
}

void Playlist::clear() {
    entries_.clear();
    index_.clear();
    shuffleList_.clear();
    queue_.clear();
    current_.reset();
    currentRemoved_ = false;
    resumeAt_.reset();
    notify({Change::Kind::Reset, 0, 0, 0});
}

void Playlist::move(std::size_t index, std::size_t count, std::size_t destination) {
    if (count == 0 || index >= entries_.size()) {
        return;
    }
    count = std::min(count, entries_.size() - index);
    destination = std::min(destination, entries_.size() - count);
    if (destination == index) {
        return;
    }

    const auto begin = entries_.begin();
    if (destination < index) {
        std::rotate(begin + static_cast<std::ptrdiff_t>(destination),
                    begin + static_cast<std::ptrdiff_t>(index),
                    begin + static_cast<std::ptrdiff_t>(index + count));
    } else {
        std::rotate(begin + static_cast<std::ptrdiff_t>(index),
                    begin + static_cast<std::ptrdiff_t>(index + count),
                    begin + static_cast<std::ptrdiff_t>(destination + count));
    }
    reindex();
    notify({Change::Kind::Moved, index, count, destination});
}

void Playlist::update(TrackId id, const std::function<void(PlaylistEntry&)>& mutate) {
    const auto position = indexOf(id);
    if (!position) {
        return;
    }
    PlaylistEntry& entry = entries_[*position];
    const TrackId  keep  = entry.id;
    mutate(entry);
    entry.id = keep;  // the id index would break otherwise
    notify({Change::Kind::Updated, *position, 1, 0});
}

void Playlist::randomize() {
    std::shuffle(entries_.begin(), entries_.end(), rng_);
    reindex();
    notify({Change::Kind::Reset, 0, 0, 0});
}

// --- persistence --------------------------------------------------------

Playlist::Snapshot Playlist::snapshot() const {
    return Snapshot{entries_, queue_, shuffleList_, current_, repeat_, shuffle_};
}

void Playlist::restore(Snapshot state) {
    entries_ = std::move(state.entries);
    reindex();

    nextId_ = 1;
    for (const auto& entry : entries_) {
        nextId_ = std::max(nextId_, entry.id + 1);
    }

    // Ids that no longer exist would otherwise sit in the queue and the shuffle
    // order forever, since nothing removes them: a stored playlist can name an
    // entry that a later version of the file no longer produces.
    const auto keepKnown = [this](std::vector<TrackId>& list) {
        std::erase_if(list, [this](TrackId id) { return !index_.contains(id); });
    };
    queue_ = std::move(state.queue);
    keepKnown(queue_);
    shuffleList_ = std::move(state.shuffleOrder);
    keepKnown(shuffleList_);

    current_ = (state.current && index_.contains(*state.current)) ? state.current
                                                                 : std::nullopt;
    currentRemoved_ = false;
    resumeAt_.reset();

    repeat_  = state.repeat;
    shuffle_ = state.shuffle;

    renumberQueue();
    for (std::size_t i = 0; i < shuffleList_.size(); ++i) {
        entries_[index_[shuffleList_[i]]].shuffleIndex = static_cast<std::int64_t>(i);
    }

    notify({Change::Kind::Reset, 0, 0, 0});
}

// --- modes --------------------------------------------------------------

void Playlist::setRepeat(RepeatMode mode) {
    if (repeat_ == mode) {
        return;
    }
    repeat_ = mode;
    notify({Change::Kind::Order, 0, 0, 0});
}

void Playlist::setShuffle(ShuffleMode mode) {
    if (shuffle_ == mode) {
        return;
    }
    shuffle_ = mode;
    if (mode == ShuffleMode::Off) {
        shuffleList_.clear();
    } else {
        resetShuffleList();
    }
    notify({Change::Kind::Order, 0, 0, 0});
}

// --- current ------------------------------------------------------------

void Playlist::setCurrent(std::optional<TrackId> id) {
    if (id && !find(*id)) {
        return;
    }
    if (current_ == id) {
        return;
    }

    if (const auto position = current_ ? indexOf(*current_) : std::nullopt) {
        PlaylistEntry& previous = entries_[*position];
        previous.currentPosition = 0.0;
        previous.stopAfter       = false;
    }

    current_        = id;
    currentRemoved_ = false;
    resumeAt_.reset();
    notify({Change::Kind::Current, 0, 0, 0});
}

// --- queue --------------------------------------------------------------

void Playlist::renumberQueue() {
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        if (const auto position = indexOf(queue_[i])) {
            entries_[*position].queuePosition = static_cast<std::int32_t>(i);
        }
    }
}

void Playlist::enqueue(TrackId id) {
    const auto position = indexOf(id);
    if (!position || entries_[*position].queued()) {
        return;
    }
    queue_.push_back(id);
    renumberQueue();
    notify({Change::Kind::Queue, 0, 0, 0});
    notify({Change::Kind::Updated, *position, 1, 0});
}

void Playlist::dequeue(TrackId id) {
    const auto it = std::find(queue_.begin(), queue_.end(), id);
    if (it == queue_.end()) {
        return;
    }
    queue_.erase(it);
    if (const auto position = indexOf(id)) {
        entries_[*position].queuePosition = -1;
    }
    renumberQueue();
    notify({Change::Kind::Queue, 0, 0, 0});
}

void Playlist::clearQueue() {
    if (queue_.empty()) {
        return;
    }
    for (const TrackId id : queue_) {
        if (const auto position = indexOf(id)) {
            entries_[*position].queuePosition = -1;
        }
    }
    queue_.clear();
    notify({Change::Kind::Queue, 0, 0, 0});
}

// --- shuffle ------------------------------------------------------------

std::vector<TrackId> Playlist::shuffledOrder() {
    std::vector<TrackId> order;
    order.reserve(entries_.size());
    for (const auto& entry : entries_) {
        order.push_back(entry.id);
    }
    // Cog pairs each track with a random long and sorts (Playlist/Shuffle.m).
    // That is a sort-based shuffle with a ~2^-32 chance of a tie biasing the
    // result; a straight Fisher-Yates is the same idea done exactly.
    std::shuffle(order.begin(), order.end(), rng_);
    return order;
}

std::vector<std::size_t> Playlist::indicesForAlbum(std::string_view album) const {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (sameAlbum(entries_[i].album, album)) {
            found.push_back(i);
        }
    }
    return found;
}

std::vector<TrackId> Playlist::albumShuffledOrder() {
    // Distinct albums in first-seen order, so the permutation depends only on
    // the RNG and not on hash iteration order -- seeding a test would otherwise
    // not reproduce.
    std::vector<std::string> albums;
    for (const auto& entry : entries_) {
        const bool seen = std::any_of(albums.begin(), albums.end(),
                                      [&entry](const std::string& album) {
                                          return sameAlbum(album, entry.album);
                                      });
        if (!seen) {
            albums.push_back(entry.album);
        }
    }
    std::shuffle(albums.begin(), albums.end(), rng_);

    std::vector<TrackId> order;
    order.reserve(entries_.size());
    for (const std::string& album : albums) {
        std::vector<std::size_t> members = indicesForAlbum(album);
        // Within an album, disc then track -- the album's own running order.
        std::stable_sort(members.begin(), members.end(),
                         [this](std::size_t lhs, std::size_t rhs) {
                             const auto& a = entries_[lhs];
                             const auto& b = entries_[rhs];
                             if (a.disc != b.disc) {
                                 return a.disc < b.disc;
                             }
                             return a.track < b.track;
                         });
        for (const std::size_t member : members) {
            order.push_back(entries_[member].id);
        }
    }
    return order;
}

void Playlist::addShuffledListToFront() {
    std::vector<TrackId> block = (shuffle_ == ShuffleMode::Albums) ? albumShuffledOrder()
                                                                   : shuffledOrder();
    shuffleList_.insert(shuffleList_.begin(), block.begin(), block.end());
    for (std::size_t i = 0; i < shuffleList_.size(); ++i) {
        if (const auto position = indexOf(shuffleList_[i])) {
            entries_[*position].shuffleIndex = static_cast<std::int64_t>(i);
        }
    }
}

void Playlist::addShuffledListToBack() {
    std::vector<TrackId> block = (shuffle_ == ShuffleMode::Albums) ? albumShuffledOrder()
                                                                   : shuffledOrder();
    const std::size_t start = shuffleList_.size();
    shuffleList_.insert(shuffleList_.end(), block.begin(), block.end());
    for (std::size_t i = start; i < shuffleList_.size(); ++i) {
        if (const auto position = indexOf(shuffleList_[i])) {
            entries_[*position].shuffleIndex = static_cast<std::int64_t>(i);
        }
    }
}

void Playlist::resetShuffleList() {
    shuffleList_.clear();
    addShuffledListToFront();

    if (!current_ || !find(*current_)) {
        return;
    }

    // The playing track has to lead the new order, or turning shuffle on
    // restarts whatever is playing at a random point in the middle of it.
    if (shuffle_ == ShuffleMode::Albums) {
        const std::string    album = find(*current_)->album;
        std::vector<TrackId> wholeAlbum;
        for (const std::size_t position : indicesForAlbum(album)) {
            wholeAlbum.push_back(entries_[position].id);
        }
        const std::unordered_set<TrackId> members(wholeAlbum.begin(), wholeAlbum.end());
        std::erase_if(shuffleList_,
                      [&members](TrackId id) { return members.contains(id); });
        shuffleList_.insert(shuffleList_.begin(), wholeAlbum.begin(), wholeAlbum.end());
    } else {
        std::erase(shuffleList_, *current_);
        shuffleList_.insert(shuffleList_.begin(), *current_);
    }

    for (std::size_t i = 0; i < shuffleList_.size(); ++i) {
        if (const auto position = indexOf(shuffleList_[i])) {
            entries_[*position].shuffleIndex = static_cast<std::int64_t>(i);
        }
    }
}

std::optional<TrackId> Playlist::shuffledEntryAt(std::int64_t i) {
    if (entries_.empty()) {
        return std::nullopt;
    }

    while (i < 0) {
        if (repeat_ != RepeatMode::All) {
            return std::nullopt;
        }
        addShuffledListToFront();
        i += static_cast<std::int64_t>(entries_.size());
    }
    while (i >= static_cast<std::int64_t>(shuffleList_.size())) {
        if (repeat_ != RepeatMode::All) {
            return std::nullopt;
        }
        addShuffledListToBack();
    }

    return shuffleList_[static_cast<std::size_t>(i)];
}

// --- traversal ----------------------------------------------------------

std::optional<TrackId> Playlist::firstTrack() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    if (shuffle_ != ShuffleMode::Off) {
        if (shuffleList_.empty()) {
            resetShuffleList();
        }
        return shuffledEntryAt(0);
    }
    return entries_.front().id;
}

std::optional<TrackId> Playlist::lastTrack() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    if (shuffle_ != ShuffleMode::Off) {
        if (shuffleList_.empty()) {
            resetShuffleList();
        }
        return shuffleList_.empty() ? std::nullopt
                                    : std::optional{shuffleList_.back()};
    }
    return entries_.back().id;
}

std::optional<TrackId> Playlist::nextEntry(TrackId from, bool ignoreRepeatOne) {
    if (entries_.empty()) {
        return std::nullopt;
    }

    if (!ignoreRepeatOne) {
        if (stopAfterCurrent_) {
            return std::nullopt;
        }
        if (repeat_ == RepeatMode::One && find(from) != nullptr) {
            return from;
        }
    }

    // The queue outranks every ordering mode: it is the user saying "this one
    // next", and it survives a shuffle or a sort.
    if (!queue_.empty()) {
        const TrackId id = queue_.front();
        queue_.erase(queue_.begin());
        if (const auto position = indexOf(id)) {
            entries_[*position].queuePosition = -1;
        }
        renumberQueue();
        notify({Change::Kind::Queue, 0, 0, 0});
        return id;
    }

    if (shuffle_ != ShuffleMode::Off) {
        if (shuffleList_.empty()) {
            resetShuffleList();
        }
        const PlaylistEntry* entry = find(from);
        const std::int64_t   here  = entry ? entry->shuffleIndex : -1;
        return shuffledEntryAt(here + 1);
    }

    const PlaylistEntry* entry = find(from);

    std::int64_t i = 0;
    if (entry != nullptr) {
        i = static_cast<std::int64_t>(*indexOf(from)) + 1;
    } else if (currentRemoved_) {
        // The entry we were playing is gone. Carry on from whatever took its
        // place -- or stop, if it was the last one. Falling back to index 0 (as
        // Cog does) restarts the playlist, which is not what deleting the last
        // track should do.
        currentRemoved_ = false;
        const auto position = resumeAt_ ? indexOf(*resumeAt_) : std::nullopt;
        resumeAt_.reset();
        i = position ? static_cast<std::int64_t>(*position)
                     : static_cast<std::int64_t>(entries_.size());
    }

    if (repeat_ == RepeatMode::Album && entry != nullptr) {
        const std::string album = entry->album;

        const bool pastEnd = i >= static_cast<std::int64_t>(entries_.size());
        const bool leftAlbum =
            !pastEnd && !sameAlbum(entries_[static_cast<std::size_t>(i)].album, album);

        if (pastEnd || leftAlbum) {
            const std::vector<std::size_t> members = indicesForAlbum(album);
            if (members.empty()) {
                return std::nullopt;
            }
            // Cog additionally repeats the single track when the album tag is
            // absent (PlaylistController.m:1512). Looping the untagged group
            // instead keeps album-repeat distinguishable from repeat-one, which
            // is the whole point of having both.
            i = static_cast<std::int64_t>(members.front());
        }
    }

    if (i >= static_cast<std::int64_t>(entries_.size())) {
        if (repeat_ != RepeatMode::All) {
            return std::nullopt;
        }
        i = 0;
    }
    return entries_[static_cast<std::size_t>(i)].id;
}

std::optional<TrackId> Playlist::previousEntry(TrackId from, bool ignoreRepeatOne) {
    if (entries_.empty()) {
        return std::nullopt;
    }

    if (!ignoreRepeatOne) {
        if (stopAfterCurrent_) {
            return std::nullopt;
        }
        if (repeat_ == RepeatMode::One && find(from) != nullptr) {
            return from;
        }
    }

    if (shuffle_ != ShuffleMode::Off) {
        if (shuffleList_.empty()) {
            resetShuffleList();
        }
        const PlaylistEntry* entry = find(from);
        const std::int64_t   here  = entry ? entry->shuffleIndex : 0;
        return shuffledEntryAt(here - 1);
    }

    std::int64_t i = -1;
    if (const auto position = indexOf(from)) {
        i = static_cast<std::int64_t>(*position) - 1;
    } else if (currentRemoved_) {
        currentRemoved_ = false;
        const auto resume = resumeAt_ ? indexOf(*resumeAt_) : std::nullopt;
        resumeAt_.reset();
        i = resume ? static_cast<std::int64_t>(*resume) - 1
                   : static_cast<std::int64_t>(entries_.size()) - 1;
    }

    if (i < 0) {
        if (repeat_ != RepeatMode::All) {
            return std::nullopt;
        }
        i = static_cast<std::int64_t>(entries_.size()) - 1;
    }
    return entries_[static_cast<std::size_t>(i)].id;
}

std::optional<TrackId> Playlist::nextForPlayback() {
    if (!current_ && !currentRemoved_) {
        return firstTrack();
    }
    // A removed current entry still has a successor to find, so `from` stays the
    // id that is no longer there and nextEntry() picks up resumeAt_.
    return nextEntry(current_.value_or(kInvalidTrackId), /*ignoreRepeatOne=*/false);
}

bool Playlist::next() {
    if (entries_.empty()) {
        return false;
    }
    if (!current_ && !currentRemoved_) {
        const auto id = firstTrack();
        if (!id) {
            return false;
        }
        setCurrent(*id);
        return true;
    }
    const auto id = nextEntry(current_.value_or(kInvalidTrackId),
                              /*ignoreRepeatOne=*/true);
    if (!id) {
        return false;
    }
    setCurrent(*id);
    return true;
}

bool Playlist::previous() {
    if (entries_.empty()) {
        return false;
    }
    if (!current_ && !currentRemoved_) {
        const auto id = lastTrack();
        if (!id) {
            return false;
        }
        setCurrent(*id);
        return true;
    }
    const auto id = previousEntry(current_.value_or(kInvalidTrackId),
                                  /*ignoreRepeatOne=*/true);
    if (!id) {
        return false;
    }
    setCurrent(*id);
    return true;
}

std::optional<TrackId> Playlist::peekPrevious() {
    if (entries_.empty()) {
        return std::nullopt;
    }
    if (!current_ && !currentRemoved_) {
        return lastTrack();
    }
    // Shuffle can extend the order here, which is a side effect -- but it is the
    // same order the eventual previous() would produce, so the answer is stable.
    return previousEntry(current_.value_or(kInvalidTrackId), /*ignoreRepeatOne=*/true);
}

}  // namespace xpcog
