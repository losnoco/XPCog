// Undo, without a toolkit.
//
// This replaces QUndoStack/QUndoCommand, which is the one piece of Qt the
// application leaned on that has no equivalent anywhere else -- wxWidgets has no
// command stack at all. It is small enough that writing it costs less than
// finding a library for it.
//
// In core rather than in the application, and that placement is doing real work:
// it lets PlaylistCommands come down here with it, which in turn moves those
// commands' tests into the suite that runs without a display. An undo stack over
// a Playlist is a domain concept, and core already owns Playlist, Signal and
// Settings.
//
// Deliberately not implemented, because nothing uses them: command merging
// (QUndoCommand::id/mergeWith), macros, and the clean-state marker. Each is easy
// to add and none should be added speculatively.
//
// The command labels are plain strings and are *not* translated here. Core has no
// catalogue and should not grow one; the caller passes the text it wants shown,
// which is where the translation belongs anyway.

#pragma once

#include "xpcog/core/Signal.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xpcog {

class UndoCommand {
public:
    explicit UndoCommand(std::string text) : text_(std::move(text)) {}

    UndoCommand(const UndoCommand&)            = delete;
    UndoCommand& operator=(const UndoCommand&) = delete;

    virtual ~UndoCommand() = default;

    /// Performs the edit. Called once by push(), and again by each redo().
    ///
    /// Must be repeatable: pushing performs it, so a command whose second run
    /// differs from its first makes redo mean something other than "do that
    /// again". RandomizeCommand is the case to look at -- it captures its
    /// permutation on the first run precisely so the second replays it.
    virtual void redo() = 0;

    /// Reverses it. Only ever called after a redo().
    virtual void undo() = 0;

    /// What to show beside Undo and Redo. Already in the user's language.
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::string text_;
};

class UndoStack {
public:
    /// Performs `command` and makes it undoable, discarding anything that had
    /// been undone -- the usual rule: a new edit after an undo abandons the
    /// branch that was undone.
    void push(std::unique_ptr<UndoCommand> command);

    void undo();
    void redo();

    /// Forgets everything, without undoing any of it. For a state change that is
    /// not an edit -- loading a saved playlist at startup, which must not be the
    /// first thing Undo offers to take back.
    void clear();

    [[nodiscard]] bool canUndo() const noexcept { return index_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return index_ < commands_.size(); }

    /// The label of the command Undo or Redo would act on, or empty when there
    /// is none.
    [[nodiscard]] std::string undoText() const;
    [[nodiscard]] std::string redoText() const;

    /// Anything about the stack changed: pushed, undone, redone or cleared.
    ///
    /// One signal rather than QUndoStack's several, because every subscriber
    /// wanted all of them: the enabled state and the labels of two menu items
    /// are the whole audience.
    Signal<> changed;

private:
    std::vector<std::unique_ptr<UndoCommand>> commands_;

    /// How many of `commands_` have been performed. Everything below it can be
    /// undone; everything from it up can be redone.
    std::size_t index_ = 0;
};

}  // namespace xpcog
