#include "xpcog/core/UndoStack.hpp"

namespace xpcog {

void UndoStack::push(std::unique_ptr<UndoCommand> command) {
    if (!command) {
        return;
    }

    // The redo branch goes before the new command runs, not after. Doing it the
    // other way round leaves the discarded commands holding state -- removed
    // entries, in RemoveTracksCommand's case -- alive across an edit that may
    // invalidate it.
    commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(index_),
                    commands_.end());

    command->redo();
    commands_.push_back(std::move(command));
    index_ = commands_.size();

    changed.publish();
}

void UndoStack::undo() {
    if (!canUndo()) {
        return;
    }
    --index_;
    commands_[index_]->undo();
    changed.publish();
}

void UndoStack::redo() {
    if (!canRedo()) {
        return;
    }
    commands_[index_]->redo();
    ++index_;
    changed.publish();
}

void UndoStack::clear() {
    commands_.clear();
    index_ = 0;
    changed.publish();
}

std::string UndoStack::undoText() const {
    return canUndo() ? commands_[index_ - 1]->text() : std::string{};
}

std::string UndoStack::redoText() const {
    return canRedo() ? commands_[index_]->text() : std::string{};
}

}  // namespace xpcog
