// The undo stack, on its own.
//
// PlaylistCommands exercises it against a real playlist; this covers the stack's
// own bookkeeping, which is the part with an off-by-one in it if anywhere does.

#include "xpcog/core/UndoStack.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

/// Appends a letter on redo and takes it off on undo, so the log is a readable
/// picture of what ran and in which direction.
class Letter : public UndoCommand {
public:
    Letter(std::string& log, char letter, std::string text)
        : UndoCommand(std::move(text)), log_(log), letter_(letter) {}

    void redo() override { log_ += letter_; }
    void undo() override {
        if (!log_.empty()) {
            log_.pop_back();
        }
    }

private:
    std::string& log_;
    char         letter_;
};

std::unique_ptr<Letter> letter(std::string& log, char c) {
    return std::make_unique<Letter>(log, c, std::string{"Type "} + c);
}

}  // namespace

TEST_CASE("pushing performs the command", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    stack.push(letter(log, 'a'));

    CHECK(log == "a");
    CHECK(stack.canUndo());
    CHECK_FALSE(stack.canRedo());
}

TEST_CASE("undo and redo walk the stack", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    stack.push(letter(log, 'a'));
    stack.push(letter(log, 'b'));
    stack.push(letter(log, 'c'));
    REQUIRE(log == "abc");

    stack.undo();
    stack.undo();
    CHECK(log == "a");
    CHECK(stack.canUndo());
    CHECK(stack.canRedo());

    stack.redo();
    CHECK(log == "ab");

    stack.redo();
    CHECK(log == "abc");
    CHECK_FALSE(stack.canRedo());
}

TEST_CASE("undoing past the bottom or redoing past the top does nothing",
          "[core][undo]") {
    std::string log;
    UndoStack   stack;

    // Both are reachable from the interface: a menu item can be clicked between
    // the state changing and the enabled state catching up.
    stack.undo();
    stack.redo();
    CHECK(log.empty());

    stack.push(letter(log, 'a'));
    stack.undo();
    stack.undo();
    CHECK(log.empty());

    stack.redo();
    stack.redo();
    CHECK(log == "a");
}

TEST_CASE("a new edit after an undo discards the redo branch", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    stack.push(letter(log, 'a'));
    stack.push(letter(log, 'b'));
    stack.undo();
    REQUIRE(log == "a");
    REQUIRE(stack.canRedo());

    stack.push(letter(log, 'c'));

    CHECK(log == "ac");
    CHECK_FALSE(stack.canRedo());
    CHECK(stack.undoText() == "Type c");
}

TEST_CASE("the labels follow the position in the stack", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    CHECK(stack.undoText().empty());
    CHECK(stack.redoText().empty());

    stack.push(letter(log, 'a'));
    stack.push(letter(log, 'b'));
    CHECK(stack.undoText() == "Type b");
    CHECK(stack.redoText().empty());

    stack.undo();
    CHECK(stack.undoText() == "Type a");
    CHECK(stack.redoText() == "Type b");
}

TEST_CASE("clear forgets without undoing", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    stack.push(letter(log, 'a'));
    stack.push(letter(log, 'b'));
    stack.clear();

    // The edits stand; only the ability to take them back is gone. That is what
    // loading a saved playlist at startup needs -- it is not an edit the user
    // made, and must not be the first thing Undo offers to reverse.
    CHECK(log == "ab");
    CHECK_FALSE(stack.canUndo());
    CHECK_FALSE(stack.canRedo());
}

TEST_CASE("every change announces itself exactly once", "[core][undo]") {
    std::string log;
    UndoStack   stack;

    int                changes      = 0;
    const Subscription subscription = stack.changed.connect([&] { ++changes; });

    stack.push(letter(log, 'a'));
    CHECK(changes == 1);

    stack.undo();
    CHECK(changes == 2);

    stack.redo();
    CHECK(changes == 3);

    stack.clear();
    CHECK(changes == 4);

    // Refused operations are not changes: a menu item that relabels itself when
    // nothing happened is noise, and this signal drives exactly that.
    stack.undo();
    stack.redo();
    CHECK(changes == 4);
}

TEST_CASE("a null command is ignored rather than stored", "[core][undo]") {
    UndoStack stack;
    stack.push(nullptr);
    CHECK_FALSE(stack.canUndo());
}
