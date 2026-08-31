// The playlist edits, with the selection taken out of them.
//
// Every one of these lived in MainFrame as a private method that began by asking
// the wxDataViewCtrl what was selected. That was fine while the window was the
// only thing that could edit a playlist. The REST remote control is the second,
// and it has no selection to read -- it names the tracks it means.
//
// So the bodies moved here and take their ids as an argument. MainFrame's
// handlers are now one line each, passing selectedTracks(); the remote layer
// passes what the request said. Neither one duplicates the other, and the remote
// side never synthesises a wxCommandEvent or touches a widget.
//
// This lives in app/ rather than core/ because the undo labels are user-visible
// text and belong in the layer that has a catalogue -- which is what UndoStack's
// own header already says: "the caller passes the text it wants shown, which is
// where the translation belongs anyway".
//
// --- What is undoable, and what is not -------------------------------------
//
// The structural edits are: insert, remove, move, randomize, clear. The
// per-entry flags are not: queueing a track, marking it stop-after, clearing a
// play count or a rating. That split is not a judgement about what deserves
// undo, it is what the window already does -- none of those four is undoable
// from the menu either. Matching the window is the rule here; "everything is
// undoable" would be a behaviour change smuggled in under a refactor.

#pragma once

#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"

#include <cstddef>
#include <vector>

namespace xpcog {
class Library;
class UndoStack;
}  // namespace xpcog

namespace xpcog::app {

/// Who asked. It reaches only the undo label.
///
/// A remote edit lands on the same undo stack the Edit menu drives, which is the
/// point -- a track deleted from a phone is undone with Ctrl+Z like any other.
/// But an unmarked "Undo Remove 3 Tracks" for something the user did not do is a
/// small mystery, so the label says where it came from.
enum class Origin { Local, Remote };

class AppCommands {
public:
    /// `library` may be null: a build or a session without one still edits a
    /// playlist, it just has nowhere to put a play count.
    AppCommands(Playlist& playlist, UndoStack& undo, Library* library);

    /// The library arrives after this does. MainFrame opens it in its
    /// constructor's body -- reporting the failure needs a frame to report it on
    /// -- and drops it when it will not open, so the pointer cannot be settled
    /// in an initialiser list.
    void setLibrary(Library* library) noexcept { library_ = library; }

    // --- Structural, undoable ----------------------------------------------

    /// Adds entries at `at`, clamped to the end. Returns the ids they were given.
    std::vector<TrackId> insert(std::vector<PlaylistEntry> entries, std::size_t at,
                                Origin origin = Origin::Local);

    /// Returns how many were removed.
    std::size_t remove(std::vector<TrackId> ids, Origin origin = Origin::Local);

    /// Removes tracks whose files went to the trash, with the wording that says
    /// so. Separate from remove() only because the label is not the same
    /// sentence, and the label is the whole of what Origin would otherwise pick.
    std::size_t removeTrashed(std::vector<TrackId> ids);

    /// Moves `ids` to sit before `anchor`, or to the end for kInvalidTrackId.
    /// False when the move would change nothing.
    bool move(const std::vector<TrackId>& ids, TrackId anchor,
              Origin origin = Origin::Local);

    /// False when there is nothing to shuffle.
    bool randomize(Origin origin = Origin::Local);

    /// Returns how many were removed.
    std::size_t clear(Origin origin = Origin::Local);

    // --- Per-entry flags, not undoable -------------------------------------

    std::size_t setQueued(const std::vector<TrackId>& ids, bool queued);

    /// Per entry rather than one decision for the whole set, which is Cog's
    /// -toggleQueuedForEntries: (PlaylistController.m:1909). A mixed selection
    /// ends up inverted rather than made uniform, and the menu label says so.
    std::size_t toggleQueued(const std::vector<TrackId>& ids);

    void clearQueue();

    std::size_t setStopAfter(const std::vector<TrackId>& ids, bool stopAfter);
    std::size_t toggleStopAfter(const std::vector<TrackId>& ids);

    /// Clears the count in the database and on the entry -- the database alone
    /// would leave the old number on screen until the next launch.
    std::size_t resetPlayCount(const std::vector<TrackId>& ids);

    /// Zero, which is how a rating is cleared. Nothing without a library.
    std::size_t removeRating(const std::vector<TrackId>& ids);

    // --- The stack ---------------------------------------------------------

    void undo();
    void redo();

private:
    Playlist&  playlist_;
    UndoStack& undo_;
    Library*   library_ = nullptr;
};

}  // namespace xpcog::app
