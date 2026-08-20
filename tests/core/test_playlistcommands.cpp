// Undoable playlist edits.
//
// The interesting property throughout is that undo restores the *same* entries,
// not equal ones: ids have to survive, because the queue and the playing track
// hold them.

#include "xpcog/core/library/PlaylistCommands.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

Playlist makeTracks(int count) {
    Playlist                   playlist;
    std::vector<PlaylistEntry> entries;
    for (int i = 1; i <= count; ++i) {
        PlaylistEntry entry;
        entry.url = *Url::parse("file:///music/" + std::to_string(i) + ".flac");
        entry.rawTitle = std::to_string(i);
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));
    return playlist;
}

/// The titles in order, which double as a readable picture of the ordering.
std::string picture(const Playlist& playlist) {
    std::string text;
    for (const PlaylistEntry& entry : playlist.entries()) {
        if (!text.empty()) {
            text += ' ';
        }
        text += entry.rawTitle;
    }
    return text;
}

std::vector<PlaylistEntry> twoNewTracks() {
    std::vector<PlaylistEntry> entries;
    for (const char* name : {"a", "b"}) {
        PlaylistEntry entry;
        entry.url      = *Url::parse(std::string{"file:///music/"} + name + ".flac");
        entry.rawTitle = name;
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace

TEST_CASE("adding tracks undoes and redoes", "[app][undo]") {
    Playlist   playlist = makeTracks(3);
    UndoStack stack;

    auto  owned   = std::make_unique<InsertTracksCommand>(playlist, 1, twoNewTracks(), "Add");
    auto* command = owned.get();
    stack.push(std::move(owned));

    REQUIRE(picture(playlist) == "1 a b 2 3");
    const std::vector<TrackId> assigned = command->ids();
    REQUIRE(assigned.size() == 2);

    stack.undo();
    CHECK(picture(playlist) == "1 2 3");

    stack.redo();
    CHECK(picture(playlist) == "1 a b 2 3");
    // The same ids, not merely the same titles: anything that referred to the
    // re-added entries still refers to them.
    CHECK(playlist.at(1).id == assigned[0]);
    CHECK(playlist.at(2).id == assigned[1]);
}

TEST_CASE("removing a scattered selection undoes in one step", "[app][undo]") {
    Playlist   playlist = makeTracks(6);
    UndoStack stack;

    const TrackId second = playlist.at(1).id;
    const TrackId fourth = playlist.at(3).id;
    const TrackId fifth  = playlist.at(4).id;

    stack.push(std::make_unique<RemoveTracksCommand>(
        playlist, std::vector<TrackId>{second, fourth, fifth}, "Remove"));
    REQUIRE(picture(playlist) == "1 3 6");

    stack.undo();
    CHECK(picture(playlist) == "1 2 3 4 5 6");
    CHECK(playlist.at(1).id == second);
    CHECK(playlist.at(3).id == fourth);
    CHECK(playlist.at(4).id == fifth);

    stack.redo();
    CHECK(picture(playlist) == "1 3 6");
}

TEST_CASE("undoing a removal restores the queue", "[app][undo]") {
    Playlist   playlist = makeTracks(4);
    UndoStack stack;

    const TrackId second = playlist.at(1).id;
    const TrackId third  = playlist.at(2).id;
    playlist.enqueue(third);
    playlist.enqueue(second);
    REQUIRE(playlist.queue() == std::vector<TrackId>{third, second});

    // Removing a queued entry drops it from the queue and renumbers the rest.
    // Putting the entry back is not enough on its own.
    stack.push(std::make_unique<RemoveTracksCommand>(playlist, std::vector<TrackId>{second}, "Remove"));
    REQUIRE(playlist.queue() == std::vector<TrackId>{third});

    stack.undo();
    CHECK(playlist.queue() == std::vector<TrackId>{third, second});
    CHECK(playlist.at(1).queuePosition == 1);
}

TEST_CASE("a multi-row drag lands in the order it was picked up", "[app][undo]") {
    Playlist playlist = makeTracks(6);

    // Rows 2 and 4 dragged onto row 6. The old implementation performed this as
    // a sequence of single moves and had to adjust the target between them; the
    // adjustment is what used to reverse the selection.
    const std::vector<TrackId> moved{playlist.at(1).id, playlist.at(3).id};
    const TrackId              anchor = playlist.at(5).id;

    UndoStack stack;
    stack.push(std::make_unique<ReorderCommand>(playlist, orderAfterMove(playlist, moved, anchor),
                                  "Move"));
    CHECK(picture(playlist) == "1 3 5 2 4 6");

    stack.undo();
    CHECK(picture(playlist) == "1 2 3 4 5 6");
}

TEST_CASE("dragging past the last row appends", "[app][undo]") {
    Playlist playlist = makeTracks(4);

    const std::vector<TrackId> moved{playlist.at(0).id};
    playlist.reorder(orderAfterMove(playlist, moved, kInvalidTrackId));
    CHECK(picture(playlist) == "2 3 4 1");
}

TEST_CASE("dropping a selection onto itself changes nothing meaningful",
          "[app][undo]") {
    Playlist playlist = makeTracks(4);

    // The anchor is one of the dragged rows, so it is lifted along with them
    // and there is nothing to insert before.
    const std::vector<TrackId> moved{playlist.at(1).id, playlist.at(2).id};
    playlist.reorder(orderAfterMove(playlist, moved, playlist.at(2).id));
    CHECK(picture(playlist) == "1 4 2 3");
}

TEST_CASE("redoing a randomize replays the same permutation", "[app][undo]") {
    Playlist playlist = makeTracks(12);
    playlist.seedShuffle(1234);

    UndoStack stack;
    stack.push(std::make_unique<RandomizeCommand>(playlist, "Randomize"));

    const std::string shuffled = picture(playlist);
    REQUIRE(shuffled != "1 2 3 4 5 6 7 8 9 10 11 12");

    stack.undo();
    CHECK(picture(playlist) == "1 2 3 4 5 6 7 8 9 10 11 12");

    // Redo must be the same edit. Calling randomize() again would draw a new
    // permutation, which is a different edit wearing the same label.
    stack.redo();
    CHECK(picture(playlist) == shuffled);
}

TEST_CASE("the undo stack reports what it would take back", "[app][undo]") {
    Playlist   playlist = makeTracks(2);
    UndoStack stack;

    CHECK_FALSE(stack.canUndo());
    stack.push(std::make_unique<InsertTracksCommand>(playlist, 0, twoNewTracks(),
                                       "Add 2 Tracks"));
    CHECK(stack.canUndo());
    CHECK(stack.undoText() == "Add 2 Tracks");

    stack.undo();
    CHECK_FALSE(stack.canUndo());
    CHECK(stack.redoText() == "Add 2 Tracks");
}
