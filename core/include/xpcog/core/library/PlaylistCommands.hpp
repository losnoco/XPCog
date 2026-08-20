// Undoable playlist edits.
//
// Cog has no undo at all: -delete: removes the managed objects and the only way
// back is to re-add the files. Every edit here goes through an UndoCommand
// instead, which costs one push() per mutation at the call site.
//
// Two things make this less trivial than "call the inverse mutator":
//
//  * Undoing a removal must bring back the *same* entries. Playlist hands out
//    ids on insert, and the queue and the playing track hold ids, so restoring
//    with fresh ones would silently unlink both. Playlist::reinsert exists for
//    that and is not used anywhere else.
//  * A drag that moves several rows, and randomize, are one user action each
//    but many moves. Their undo is expressed as a target order rather than as a
//    sequence of inverse moves, so no inverse ever has to be derived.
//
// Commands take a Playlist reference and must not outlive it. The undo stack is
// owned by the window that owns the playlist, so that holds.
//
// The label each command carries is passed in rather than composed here. Core has
// no message catalogue, and the caller is where the user's language is known --
// which is also why RandomizeCommand takes one where it used to build its own.

#pragma once

#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {

/// Adds entries at a position. Holds them while undone.
class InsertTracksCommand : public UndoCommand {
public:
    InsertTracksCommand(Playlist& playlist, std::size_t index,
                        std::vector<PlaylistEntry> entries, std::string text);

    void redo() override;
    void undo() override;

    /// The ids the entries were given. Empty until the first redo(), which
    /// UndoStack::push performs.
    [[nodiscard]] const std::vector<TrackId>& ids() const noexcept { return ids_; }

private:
    Playlist&                  playlist_;
    std::size_t                index_;
    std::vector<PlaylistEntry> entries_;
    std::vector<TrackId>       ids_;
};

/// Removes entries by id. The selection need not be contiguous.
class RemoveTracksCommand : public UndoCommand {
public:
    RemoveTracksCommand(Playlist& playlist, std::vector<TrackId> ids, std::string text);

    void redo() override;
    void undo() override;

private:
    /// A contiguous stretch of removed rows and where it was. Restoring runs in
    /// ascending order puts each back before the next one's index is needed,
    /// so no index arithmetic is required between them.
    struct Run {
        std::size_t                index = 0;
        std::vector<PlaylistEntry> entries;
    };

    Playlist&            playlist_;
    std::vector<TrackId> ids_;
    std::vector<Run>     runs_;

    /// Removing a queued entry drops it from the queue and renumbers what is
    /// left. Putting the entry back does not put it back in the queue, so the
    /// queue is captured and restored wholesale.
    std::vector<TrackId> queue_;
};

/// Rearranges the whole list into a given order. Drag-and-drop reordering.
class ReorderCommand : public UndoCommand {
public:
    ReorderCommand(Playlist& playlist, std::vector<TrackId> order, std::string text);

    void redo() override;
    void undo() override;

private:
    Playlist&            playlist_;
    std::vector<TrackId> before_;
    std::vector<TrackId> after_;
};

/// Cog's -randomizeList:. Unlike shuffle, this really reorders the playlist.
class RandomizeCommand : public UndoCommand {
public:
    RandomizeCommand(Playlist& playlist, std::string text);

    void redo() override;
    void undo() override;

private:
    Playlist&            playlist_;
    std::vector<TrackId> before_;
    /// Captured from the first redo(), so redoing replays the same permutation
    /// rather than drawing a new one -- redo has to be the same edit.
    std::optional<std::vector<TrackId>> after_;
};

/// The playlist's current order as ids, for the commands that need a before or
/// an after picture.
[[nodiscard]] std::vector<TrackId> currentOrder(const Playlist& playlist);

/// Where a drag of `moved` dropped before `anchor` leaves the playlist.
///
/// Computed rather than performed: the move is a single undoable edit, so the
/// resulting order has to exist before anything is mutated. `anchor` is the id
/// of the row the selection was dropped onto, or kInvalidTrackId for the end.
[[nodiscard]] std::vector<TrackId> orderAfterMove(const Playlist& playlist,
                                                  const std::vector<TrackId>& moved,
                                                  TrackId anchor);

}  // namespace xpcog
