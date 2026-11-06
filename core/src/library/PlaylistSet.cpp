#include "xpcog/core/library/PlaylistSet.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace xpcog {

PlaylistSet::PlaylistSet() : shared_(std::make_shared<Playlist::Shared>()) {
    shared_->grouped      = true;
    shared_->queueChanged = [this] { syncQueue(); };
    append(nextId_++, {}, std::nullopt);
}

PlaylistSet::Item& PlaylistSet::append(Id id, std::string name,
                                       std::optional<std::size_t> position) {
    auto created      = std::make_unique<Item>();
    created->id       = id;
    created->name     = std::move(name);
    created->playlist = std::make_unique<Playlist>(shared_);
    created->playlist->setRepeat(repeat_);
    created->playlist->setShuffle(shuffle_);
    created->playlist->setStopAfterCurrent(stopAfter_);
    created->watch = created->playlist->observe([this, id](const Playlist::Change& change) {
        contentChanged_.publish(id, change);
    });

    const std::size_t where = std::min(position.value_or(items_.size()), items_.size());
    const auto        it =
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(where), std::move(created));
    return **it;
}

std::optional<std::size_t> PlaylistSet::indexOf(Id id) const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i]->id == id) {
            return i;
        }
    }
    return std::nullopt;
}

PlaylistSet::Item* PlaylistSet::item(Id id) {
    const auto index = indexOf(id);
    return index ? items_[*index].get() : nullptr;
}

const PlaylistSet::Item* PlaylistSet::item(Id id) const {
    const auto index = indexOf(id);
    return index ? items_[*index].get() : nullptr;
}

Playlist* PlaylistSet::playlist(Id id) {
    Item* found = item(id);
    return found != nullptr ? found->playlist.get() : nullptr;
}

const Playlist* PlaylistSet::playlist(Id id) const {
    const Item* found = item(id);
    return found != nullptr ? found->playlist.get() : nullptr;
}

PlaylistSet::Id PlaylistSet::add(std::string name, std::optional<std::size_t> position) {
    const Id id = nextId_++;
    append(id, std::move(name), position);
    publish({Change::Kind::Added, id});
    return id;
}

bool PlaylistSet::remove(Id id) {
    const auto index = indexOf(id);
    if (!index || items_.size() <= 1) {
        return false;
    }

    // Its neighbour: the one after it, or the one before when it was last.
    const std::size_t neighbour = (*index + 1 < items_.size()) ? *index + 1 : *index - 1;
    const Id          fallback  = items_[neighbour]->id;

    if (shown_ == id) {
        show(fallback);
    }
    if (playing_ == id) {
        setPlaying(fallback);
    }

    // Through clear(), so the shared queue loses what was queued from here.
    items_[*index]->playlist->clear();
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(*index));
    publish({Change::Kind::Removed, id});
    return true;
}

bool PlaylistSet::rename(Id id, std::string name) {
    Item* found = item(id);
    if (found == nullptr || found->name == name) {
        return false;
    }
    found->name = std::move(name);
    publish({Change::Kind::Renamed, id});
    return true;
}

bool PlaylistSet::move(Id id, std::size_t position) {
    const auto index = indexOf(id);
    if (!index) {
        return false;
    }
    position = std::min(position, items_.size() - 1);
    if (position == *index) {
        return false;
    }
    std::unique_ptr<Item> moving = std::move(items_[*index]);
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(*index));
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(position), std::move(moving));
    publish({Change::Kind::Moved, id});
    return true;
}

void PlaylistSet::show(Id id) {
    if (id == shown_ || item(id) == nullptr) {
        return;
    }
    shown_ = id;
    publish({Change::Kind::Shown, id});
}

void PlaylistSet::setPlaying(Id id) {
    if (id == playing_ || item(id) == nullptr) {
        return;
    }
    playing().setCurrent(std::nullopt);
    playing_ = id;
    publish({Change::Kind::Playing, id});
}

const PlaylistEntry* PlaylistSet::find(TrackId id) const {
    for (const auto& candidate : items_) {
        if (const PlaylistEntry* entry = candidate->playlist->find(id)) {
            return entry;
        }
    }
    return nullptr;
}

Playlist* PlaylistSet::ownerOf(TrackId id) {
    for (const auto& candidate : items_) {
        if (candidate->playlist->find(id) != nullptr) {
            return candidate->playlist.get();
        }
    }
    return nullptr;
}

std::optional<PlaylistSet::Id> PlaylistSet::ownerIdOf(TrackId id) const {
    for (const auto& candidate : items_) {
        if (candidate->playlist->find(id) != nullptr) {
            return candidate->id;
        }
    }
    return std::nullopt;
}

std::optional<TrackId> PlaylistSet::findByUrl(const std::string& url) const {
    const auto search = [&url](const Playlist& list) -> std::optional<TrackId> {
        for (const PlaylistEntry& entry : list.entries()) {
            if (entry.url.toString() == url) {
                return entry.id;
            }
        }
        return std::nullopt;
    };

    if (const auto found = search(playing())) {
        return found;
    }
    for (const auto& candidate : items_) {
        if (candidate->id == playing_) {
            continue;
        }
        if (const auto found = search(*candidate->playlist)) {
            return found;
        }
    }
    return std::nullopt;
}

void PlaylistSet::update(TrackId id, const std::function<void(PlaylistEntry&)>& mutate) {
    if (Playlist* owner = ownerOf(id)) {
        owner->update(id, mutate);
    }
}

std::optional<TrackId> PlaylistSet::nextForPlayback() {
    Playlist&                    from = playing();
    const std::optional<TrackId> id   = from.nextForPlayback();
    if (!id || from.find(*id) != nullptr) {
        return id;
    }

    // Queued from another playlist. The cursor crosses now, but the row marked
    // as playing stays put until noteAudible(): that audio is still to come.
    const std::optional<Id> owner = ownerIdOf(*id);
    if (!owner) {
        return id;
    }
    from.forgetCursor();
    playlist(*owner)->adoptCursor(*id);
    playing_ = *owner;
    publish({Change::Kind::Playing, *owner});
    return id;
}

bool PlaylistSet::next() {
    Playlist& from = playing();

    if (!shared_->queue.empty() && from.find(shared_->queue.front()) == nullptr) {
        const TrackId id = shared_->queue.front();
        if (Playlist* owner = ownerOf(id)) {
            owner->dequeue(id);
            setCurrent(id);
            return true;
        }
    }
    return from.next();
}

void PlaylistSet::setCurrent(TrackId id) {
    const std::optional<Id> owner = ownerIdOf(id);
    if (!owner) {
        return;
    }
    setPlaying(*owner);
    playing().setCurrent(id);
}

void PlaylistSet::noteAudible(TrackId id) {
    const std::optional<Id> owner = ownerIdOf(id);
    if (!owner) {
        return;
    }
    for (const auto& candidate : items_) {
        if (candidate->id != *owner && candidate->playlist->current()) {
            candidate->playlist->setAudible(std::nullopt);
        }
    }
    playlist(*owner)->setAudible(id);
}

void PlaylistSet::setRepeat(RepeatMode mode) {
    repeat_ = mode;
    for (const auto& candidate : items_) {
        candidate->playlist->setRepeat(mode);
    }
}

void PlaylistSet::setShuffle(ShuffleMode mode) {
    shuffle_ = mode;
    for (const auto& candidate : items_) {
        candidate->playlist->setShuffle(mode);
    }
}

void PlaylistSet::setStopAfterCurrent(bool stop) {
    stopAfter_ = stop;
    for (const auto& candidate : items_) {
        candidate->playlist->setStopAfterCurrent(stop);
    }
}

void PlaylistSet::syncQueue() {
    for (const auto& candidate : items_) {
        candidate->playlist->syncQueue();
    }
}

void PlaylistSet::restore(std::vector<Stored> playlists, std::vector<TrackId> queue) {
    items_.clear();
    shared_->queue.clear();
    shared_->nextId = 1;

    if (playlists.empty()) {
        Stored first;
        first.id = kFirstId;
        first.state.repeat  = repeat_;
        first.state.shuffle = shuffle_;
        playlists.push_back(std::move(first));
    }

    nextId_ = kFirstId;
    for (Stored& stored : playlists) {
        nextId_ = std::max(nextId_, stored.id + 1);
        repeat_  = stored.state.repeat;
        shuffle_ = stored.state.shuffle;
        Item& created = append(stored.id, std::move(stored.name), std::nullopt);
        stored.state.queue.clear();
        created.playlist->restore(std::move(stored.state));
    }

    // Whichever one has a current entry is the one that was playing.
    shown_   = items_.front()->id;
    playing_ = items_.front()->id;
    for (const auto& candidate : items_) {
        if (candidate->playlist->current()) {
            playing_ = candidate->id;
            shown_   = candidate->id;
            break;
        }
    }

    std::erase_if(queue, [this](TrackId id) { return find(id) == nullptr; });
    shared_->queue = std::move(queue);
    syncQueue();

    publish({Change::Kind::Moved, shown_});
    publish({Change::Kind::Shown, shown_});
    publish({Change::Kind::Playing, playing_});
}

}  // namespace xpcog
