#include "PlaylistCommands.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace xpcog::app {

std::vector<TrackId> currentOrder(const Playlist& playlist) {
    std::vector<TrackId> order;
    order.reserve(playlist.size());
    for (const PlaylistEntry& entry : playlist.entries()) {
        order.push_back(entry.id);
    }
    return order;
}

std::vector<TrackId> orderAfterMove(const Playlist& playlist,
                                    const std::vector<TrackId>& moved, TrackId anchor) {
    const std::unordered_set<TrackId> lifted(moved.begin(), moved.end());

    // Lift the dragged rows out, then put them back in front of the anchor.
    // Doing it this way rather than as a sequence of Playlist::move calls
    // removes the index arithmetic entirely -- each individual move shifts
    // every row after it, and getting that adjustment wrong is what silently
    // reverses a multi-row drag.
    std::vector<TrackId> rest;
    rest.reserve(playlist.size());
    for (const PlaylistEntry& entry : playlist.entries()) {
        if (!lifted.contains(entry.id)) {
            rest.push_back(entry.id);
        }
    }

    // Dropping onto one of the dragged rows means the anchor was lifted too, so
    // there is nothing to insert before; the end of the list is the answer, and
    // it is also what dropping past the last row means.
    auto at = rest.end();
    if (anchor != kInvalidTrackId) {
        at = std::find(rest.begin(), rest.end(), anchor);
    }

    std::vector<TrackId> order(rest.begin(), at);
    order.insert(order.end(), moved.begin(), moved.end());
    order.insert(order.end(), at, rest.end());
    return order;
}

// --- insert -------------------------------------------------------------

InsertTracksCommand::InsertTracksCommand(Playlist& playlist, std::size_t index,
                                         std::vector<PlaylistEntry> entries,
                                         const QString& text)
    : QUndoCommand(text),
      playlist_(playlist),
      index_(index),
      entries_(std::move(entries)) {}

void InsertTracksCommand::redo() {
    if (ids_.empty()) {
        // First time: the playlist assigns the ids, and everything after this
        // -- including a later redo -- has to use those same ones.
        ids_ = playlist_.insert(index_, std::move(entries_));
        entries_.clear();
        return;
    }
    playlist_.reinsert(index_, std::move(entries_));
    entries_.clear();
}

void InsertTracksCommand::undo() {
    // Taken from the playlist rather than kept from construction, so anything
    // that filled in metadata since -- a tag read finishing, a play count --
    // survives the round trip.
    entries_.clear();
    entries_.reserve(ids_.size());
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        entries_.push_back(playlist_.at(index_ + i));
    }
    playlist_.removeAt(index_, ids_.size());
}

// --- remove -------------------------------------------------------------

RemoveTracksCommand::RemoveTracksCommand(Playlist& playlist, std::vector<TrackId> ids,
                                         const QString& text)
    : QUndoCommand(text), playlist_(playlist), ids_(std::move(ids)) {}

void RemoveTracksCommand::redo() {
    queue_ = playlist_.queue();

    std::vector<std::size_t> positions;
    positions.reserve(ids_.size());
    for (const TrackId id : ids_) {
        if (const auto position = playlist_.indexOf(id)) {
            positions.push_back(*position);
        }
    }
    std::sort(positions.begin(), positions.end());

    runs_.clear();
    for (const std::size_t position : positions) {
        if (!runs_.empty() && runs_.back().index + runs_.back().entries.size() == position) {
            runs_.back().entries.push_back(playlist_.at(position));
        } else {
            runs_.push_back(Run{position, {playlist_.at(position)}});
        }
    }

    playlist_.remove(ids_);
}

void RemoveTracksCommand::undo() {
    for (Run& run : runs_) {
        playlist_.reinsert(run.index, run.entries);
    }

    if (playlist_.queue() != queue_) {
        playlist_.clearQueue();
        for (const TrackId id : queue_) {
            playlist_.enqueue(id);
        }
    }
}

// --- reorder ------------------------------------------------------------

ReorderCommand::ReorderCommand(Playlist& playlist, std::vector<TrackId> order,
                               const QString& text)
    : QUndoCommand(text),
      playlist_(playlist),
      before_(currentOrder(playlist)),
      after_(std::move(order)) {}

void ReorderCommand::redo() { static_cast<void>(playlist_.reorder(after_)); }
void ReorderCommand::undo() { static_cast<void>(playlist_.reorder(before_)); }

RandomizeCommand::RandomizeCommand(Playlist& playlist)
    : QUndoCommand(QCoreApplication::translate("PlaylistCommands", "Randomize")),
      playlist_(playlist),
      before_(currentOrder(playlist)) {}

void RandomizeCommand::redo() {
    if (!after_) {
        playlist_.randomize();
        after_ = currentOrder(playlist_);
        return;
    }
    // Redo has to be the same edit, not another random one.
    static_cast<void>(playlist_.reorder(*after_));
}

void RandomizeCommand::undo() { static_cast<void>(playlist_.reorder(before_)); }

}  // namespace xpcog::app
