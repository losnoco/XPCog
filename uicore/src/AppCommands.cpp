#include "AppCommands.hpp"

#include "Translations.hpp"

#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"

#include <memory>
#include <utility>

namespace xpcog::app {


namespace {

/// The undo label, marked when the edit did not come from this window.
///
/// Format string rather than concatenation because the parenthesis and the word
/// inside it both move in translation, and a language that puts the marker first
/// has somewhere to put it.
std::string label(std::string text, Origin origin) {
    if (origin == Origin::Local) {
        return text;
    }
    // fmt() rather than trf(): `text` has already been through the catalogue,
    // and the wrapper has to be applied to the translated inner string rather
    // than to its msgid.
    return fmt(tr("%s (remote)"), text);
}

}  // namespace

AppCommands::AppCommands(Playlist& playlist, UndoStack& undo, Library* library)
    : lone_(&playlist), undo_(undo), library_(library) {}

AppCommands::AppCommands(PlaylistSet& playlists, UndoStack& undo, Library* library)
    : set_(&playlists), undo_(undo), library_(library) {}

Playlist& AppCommands::list() const { return set_ != nullptr ? set_->shown() : *lone_; }

Playlist* AppCommands::ownerOf(TrackId id) const {
    if (set_ != nullptr) {
        return set_->ownerOf(id);
    }
    return lone_->find(id) != nullptr ? lone_ : nullptr;
}

std::vector<TrackId> AppCommands::insert(std::vector<PlaylistEntry> entries,
                                         std::size_t at, Origin origin) {
    if (entries.empty()) {
        return {};
    }
    const std::size_t count = entries.size();

    auto command = std::make_unique<InsertTracksCommand>(
        list(), at, std::move(entries),
        label(fmt(trn("Add %zu Track", "Add %zu Tracks", count), count), origin));

    // Held before the push, because push() performs the first redo() and that is
    // what fills the ids in -- and the command is owned by the stack afterwards.
    const InsertTracksCommand* inserted = command.get();
    undo_.push(std::move(command));
    return inserted->ids();
}

std::size_t AppCommands::remove(std::vector<TrackId> ids, Origin origin) {
    if (ids.empty()) {
        return 0;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        list(), std::move(ids),
        label(fmt(trn("Remove %zu Track", "Remove %zu Tracks", count), count),
              origin)));
    return count;
}

std::size_t AppCommands::removeTrashed(std::vector<TrackId> ids) {
    if (ids.empty()) {
        return 0;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        list(), std::move(ids),
        fmt(trn("Move %zu Track to the Trash", "Move %zu Tracks to the Trash",
                count),
            count)));
    return count;
}

bool AppCommands::move(const std::vector<TrackId>& ids, TrackId anchor, Origin origin) {
    if (ids.empty()) {
        return false;
    }
    std::vector<TrackId> after = orderAfterMove(list(), ids, anchor);
    if (after == currentOrder(list())) {
        return false;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<ReorderCommand>(
        list(), std::move(after),
        label(fmt(trn("Move %zu Track", "Move %zu Tracks", count), count),
              origin)));
    return true;
}

bool AppCommands::randomize(Origin origin) {
    if (list().size() <= 1) {
        return false;
    }
    undo_.push(std::make_unique<RandomizeCommand>(list(), label(tr("Randomize"), origin)));
    return true;
}

std::size_t AppCommands::clear(Origin origin) {
    std::vector<TrackId> ids = currentOrder(list());
    if (ids.empty()) {
        return 0;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        list(), std::move(ids), label(tr("Clear Playlist"), origin)));
    return count;
}

std::size_t AppCommands::setQueued(const std::vector<TrackId>& ids, bool queued) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        Playlist* owner = ownerOf(id);
        if (owner == nullptr) {
            continue;
        }
        if (queued) {
            owner->enqueue(id);
        } else {
            owner->dequeue(id);
        }
        ++count;
    }
    return count;
}

std::size_t AppCommands::toggleQueued(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        Playlist* owner = ownerOf(id);
        if (owner == nullptr) {
            continue;
        }
        if (owner->find(id)->queued()) {
            owner->dequeue(id);
        } else {
            owner->enqueue(id);
        }
        ++count;
    }
    return count;
}

void AppCommands::clearQueue() { list().clearQueue(); }

std::size_t AppCommands::setStopAfter(const std::vector<TrackId>& ids, bool stopAfter) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (list().find(id) == nullptr) {
            continue;
        }
        list().update(id, [stopAfter](PlaylistEntry& entry) {
            entry.stopAfter = stopAfter;
        });
        ++count;
    }
    return count;
}

std::size_t AppCommands::toggleStopAfter(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (list().find(id) == nullptr) {
            continue;
        }
        list().update(id,
                         [](PlaylistEntry& entry) { entry.stopAfter = !entry.stopAfter; });
        ++count;
    }
    return count;
}

std::size_t AppCommands::resetPlayCount(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        const PlaylistEntry* entry = list().find(id);
        if (entry == nullptr) {
            continue;
        }
        if (library_ != nullptr) {
            static_cast<void>(library_->resetPlayCount(*entry));
        }
        // And on the entry, which is the copy the Info pane reads. The database
        // alone would leave the old number on screen until the next launch.
        list().update(id, [](PlaylistEntry& target) { target.playCount = 0; });
        ++count;
    }
    return count;
}

std::size_t AppCommands::removeRating(const std::vector<TrackId>& ids) {
    if (library_ == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (const PlaylistEntry* entry = list().find(id); entry != nullptr) {
            static_cast<void>(library_->setRating(*entry, 0.0F));
            ++count;
        }
    }
    return count;
}

void AppCommands::undo() { undo_.undo(); }
void AppCommands::redo() { undo_.redo(); }

}  // namespace xpcog::app
