#include "AppCommands.hpp"

#include "Text.hpp"

#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"

#include <wx/translation.h>

#include <memory>
#include <utility>

namespace xpcog::app {
namespace {

/// The undo label, marked when the edit did not come from this window.
///
/// Format string rather than concatenation because the parenthesis and the word
/// inside it both move in translation, and a language that puts the marker first
/// has somewhere to put it.
std::string label(const wxString& text, Origin origin) {
    if (origin == Origin::Local) {
        return toUtf8(text);
    }
    return toUtf8(wxString::Format(_("%s (remote)"), text));
}

}  // namespace

AppCommands::AppCommands(Playlist& playlist, UndoStack& undo, Library* library)
    : playlist_(playlist), undo_(undo), library_(library) {}

std::vector<TrackId> AppCommands::insert(std::vector<PlaylistEntry> entries,
                                         std::size_t at, Origin origin) {
    if (entries.empty()) {
        return {};
    }
    const std::size_t count = entries.size();

    auto command = std::make_unique<InsertTracksCommand>(
        playlist_, at, std::move(entries),
        label(wxString::Format(wxPLURAL("Add %zu Track", "Add %zu Tracks",
                                        static_cast<unsigned>(count)),
                               count),
              origin));

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
        playlist_, std::move(ids),
        label(wxString::Format(wxPLURAL("Remove %zu Track", "Remove %zu Tracks",
                                        static_cast<unsigned>(count)),
                               count),
              origin)));
    return count;
}

std::size_t AppCommands::removeTrashed(std::vector<TrackId> ids) {
    if (ids.empty()) {
        return 0;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        playlist_, std::move(ids),
        toUtf8(wxString::Format(wxPLURAL("Move %zu Track to the Trash",
                                         "Move %zu Tracks to the Trash",
                                         static_cast<unsigned>(count)),
                                count))));
    return count;
}

bool AppCommands::move(const std::vector<TrackId>& ids, TrackId anchor, Origin origin) {
    if (ids.empty()) {
        return false;
    }
    std::vector<TrackId> after = orderAfterMove(playlist_, ids, anchor);
    if (after == currentOrder(playlist_)) {
        return false;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<ReorderCommand>(
        playlist_, std::move(after),
        label(wxString::Format(wxPLURAL("Move %zu Track", "Move %zu Tracks",
                                        static_cast<unsigned>(count)),
                               count),
              origin)));
    return true;
}

bool AppCommands::randomize(Origin origin) {
    if (playlist_.size() <= 1) {
        return false;
    }
    undo_.push(std::make_unique<RandomizeCommand>(playlist_, label(_("Randomize"), origin)));
    return true;
}

std::size_t AppCommands::clear(Origin origin) {
    std::vector<TrackId> ids = currentOrder(playlist_);
    if (ids.empty()) {
        return 0;
    }
    const std::size_t count = ids.size();
    undo_.push(std::make_unique<RemoveTracksCommand>(
        playlist_, std::move(ids), label(_("Clear Playlist"), origin)));
    return count;
}

std::size_t AppCommands::setQueued(const std::vector<TrackId>& ids, bool queued) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        if (queued) {
            playlist_.enqueue(id);
        } else {
            playlist_.dequeue(id);
        }
        ++count;
    }
    return count;
}

std::size_t AppCommands::toggleQueued(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (entry->queued()) {
            playlist_.dequeue(id);
        } else {
            playlist_.enqueue(id);
        }
        ++count;
    }
    return count;
}

void AppCommands::clearQueue() { playlist_.clearQueue(); }

std::size_t AppCommands::setStopAfter(const std::vector<TrackId>& ids, bool stopAfter) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        playlist_.update(id, [stopAfter](PlaylistEntry& entry) {
            entry.stopAfter = stopAfter;
        });
        ++count;
    }
    return count;
}

std::size_t AppCommands::toggleStopAfter(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        playlist_.update(id,
                         [](PlaylistEntry& entry) { entry.stopAfter = !entry.stopAfter; });
        ++count;
    }
    return count;
}

std::size_t AppCommands::resetPlayCount(const std::vector<TrackId>& ids) {
    std::size_t count = 0;
    for (const TrackId id : ids) {
        const PlaylistEntry* entry = playlist_.find(id);
        if (entry == nullptr) {
            continue;
        }
        if (library_ != nullptr) {
            static_cast<void>(library_->resetPlayCount(*entry));
        }
        // And on the entry, which is the copy the Info pane reads. The database
        // alone would leave the old number on screen until the next launch.
        playlist_.update(id, [](PlaylistEntry& target) { target.playCount = 0; });
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
        if (const PlaylistEntry* entry = playlist_.find(id); entry != nullptr) {
            static_cast<void>(library_->setRating(*entry, 0.0F));
            ++count;
        }
    }
    return count;
}

void AppCommands::undo() { undo_.undo(); }
void AppCommands::redo() { undo_.redo(); }

}  // namespace xpcog::app
