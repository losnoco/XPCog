// The playlist edits, now that the selection is no longer part of them.
//
// These bodies used to live in MainFrame and began by asking a wxDataViewCtrl
// what was selected, which made them untestable without a window and unreachable
// from anything but a menu. They take their ids as an argument now, so both the
// window and the REST remote control drive the same code -- and so this suite can
// drive it with no display at all.
//
// The property worth pinning is the split: structural edits are undoable and the
// per-entry flags are not. That is not a judgement about what deserves undo, it
// is what the window already does, and a refactor that quietly widened it would
// be a behaviour change nobody asked for.

#include "AppCommands.hpp"

#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::app;

namespace {

struct Fixture {
    Playlist    playlist;
    UndoStack   undo;
    AppCommands commands{playlist, undo, nullptr};

    explicit Fixture(int count) {
        std::vector<PlaylistEntry> entries;
        for (int i = 1; i <= count; ++i) {
            PlaylistEntry entry;
            entry.url      = *Url::parse("file:///music/" + std::to_string(i) + ".flac");
            entry.rawTitle = std::to_string(i);
            entries.push_back(std::move(entry));
        }
        playlist.insert(0, std::move(entries));
    }

    [[nodiscard]] std::string picture() const {
        std::string text;
        for (const PlaylistEntry& entry : playlist.entries()) {
            if (!text.empty()) {
                text += ' ';
            }
            text += entry.rawTitle;
        }
        return text;
    }

    [[nodiscard]] std::vector<TrackId> ids(std::initializer_list<std::size_t> rows) const {
        std::vector<TrackId> picked;
        for (const std::size_t row : rows) {
            picked.push_back(playlist.at(row).id);
        }
        return picked;
    }
};

}  // namespace

TEST_CASE("removing is undoable and restores the same entries", "[playlist][remote]") {
    Fixture fixture(4);
    const std::vector<TrackId> doomed = fixture.ids({1, 2});

    REQUIRE(fixture.commands.remove(doomed) == 2);
    CHECK(fixture.picture() == "1 4");
    REQUIRE(fixture.undo.canUndo());

    fixture.commands.undo();
    CHECK(fixture.picture() == "1 2 3 4");
    // The same ids, not merely equal entries: the queue and the playing track
    // hold them.
    CHECK(fixture.ids({1, 2}) == doomed);
}

TEST_CASE("an empty selection is not an edit", "[playlist][remote]") {
    Fixture fixture(3);
    CHECK(fixture.commands.remove({}) == 0);
    CHECK_FALSE(fixture.undo.canUndo());
    CHECK(fixture.picture() == "1 2 3");
}

TEST_CASE("inserting answers with the ids it gave", "[playlist][remote]") {
    Fixture fixture(2);

    std::vector<PlaylistEntry> added;
    PlaylistEntry              entry;
    entry.url      = *Url::parse("file:///music/new.flac");
    entry.rawTitle = "new";
    added.push_back(std::move(entry));

    const std::vector<TrackId> given = fixture.commands.insert(std::move(added), 1);
    REQUIRE(given.size() == 1);
    CHECK(fixture.picture() == "1 new 2");
    CHECK(fixture.playlist.at(1).id == given.front());

    fixture.commands.undo();
    CHECK(fixture.picture() == "1 2");
}

TEST_CASE("clear is one undoable edit", "[playlist][remote]") {
    Fixture fixture(3);
    REQUIRE(fixture.commands.clear() == 3);
    CHECK(fixture.playlist.size() == 0);

    fixture.commands.undo();
    CHECK(fixture.picture() == "1 2 3");
}

TEST_CASE("moving to the end and moving before an anchor", "[playlist][remote]") {
    Fixture fixture(4);

    REQUIRE(fixture.commands.move(fixture.ids({0}), kInvalidTrackId));
    CHECK(fixture.picture() == "2 3 4 1");

    fixture.commands.undo();
    CHECK(fixture.picture() == "1 2 3 4");

    REQUIRE(fixture.commands.move(fixture.ids({3}), fixture.playlist.at(1).id));
    CHECK(fixture.picture() == "1 4 2 3");
}

TEST_CASE("a move that changes nothing is not pushed", "[playlist][remote]") {
    Fixture fixture(3);
    // Already where it would land.
    CHECK_FALSE(fixture.commands.move(fixture.ids({0}), fixture.playlist.at(1).id));
    CHECK_FALSE(fixture.undo.canUndo());
}

TEST_CASE("randomize declines a playlist too short to shuffle", "[playlist][remote]") {
    Fixture one(1);
    CHECK_FALSE(one.commands.randomize());
    CHECK_FALSE(one.undo.canUndo());

    Fixture many(8);
    CHECK(many.commands.randomize());
    CHECK(many.undo.canUndo());
    many.commands.undo();
    CHECK(many.picture() == "1 2 3 4 5 6 7 8");
}

TEST_CASE("queueing is per entry and is not undoable", "[playlist][remote]") {
    Fixture fixture(4);

    REQUIRE(fixture.commands.setQueued(fixture.ids({0, 1}), true) == 2);
    CHECK(fixture.playlist.at(0).queued());
    CHECK(fixture.playlist.at(1).queued());

    // Mixed in, mixed out: a toggle inverts each rather than making the set
    // uniform, which is Cog's -toggleQueuedForEntries:.
    REQUIRE(fixture.commands.toggleQueued(fixture.ids({1, 2})) == 2);
    CHECK(fixture.playlist.at(0).queued());
    CHECK_FALSE(fixture.playlist.at(1).queued());
    CHECK(fixture.playlist.at(2).queued());

    // None of it reached the stack.
    CHECK_FALSE(fixture.undo.canUndo());

    fixture.commands.clearQueue();
    CHECK_FALSE(fixture.playlist.at(0).queued());
    CHECK_FALSE(fixture.playlist.at(2).queued());
}

TEST_CASE("stop-after is per entry and is not undoable", "[playlist][remote]") {
    Fixture fixture(3);

    REQUIRE(fixture.commands.setStopAfter(fixture.ids({2}), true) == 1);
    CHECK(fixture.playlist.at(2).stopAfter);

    REQUIRE(fixture.commands.toggleStopAfter(fixture.ids({1, 2})) == 2);
    CHECK(fixture.playlist.at(1).stopAfter);
    CHECK_FALSE(fixture.playlist.at(2).stopAfter);

    CHECK_FALSE(fixture.undo.canUndo());
}

TEST_CASE("ids that are not in the playlist are skipped, not counted",
          "[playlist][remote]") {
    Fixture fixture(2);
    const TrackId absent = 999999;

    CHECK(fixture.commands.setQueued({absent}, true) == 0);
    CHECK(fixture.commands.setStopAfter({absent}, true) == 0);
    CHECK(fixture.commands.toggleQueued({absent}) == 0);
}

TEST_CASE("a play count reset needs no library to touch the entry",
          "[playlist][remote]") {
    Fixture fixture(2);
    fixture.playlist.update(fixture.playlist.at(0).id,
                            [](PlaylistEntry& entry) { entry.playCount = 7; });

    // Null library: the database half is skipped and the entry half is not,
    // which is what keeps the Info pane from showing the old number.
    REQUIRE(fixture.commands.resetPlayCount(fixture.ids({0})) == 1);
    CHECK(fixture.playlist.at(0).playCount == 0);

    // Ratings live only in the database, so with no library there is nothing to
    // do and nothing to claim.
    CHECK(fixture.commands.removeRating(fixture.ids({0})) == 0);
}

TEST_CASE("a remote edit is marked in the undo label", "[playlist][remote]") {
    Fixture fixture(3);

    fixture.commands.remove(fixture.ids({0}), Origin::Local);
    const std::string local = fixture.undo.undoText();

    fixture.commands.remove(fixture.ids({0}), Origin::Remote);
    const std::string remote = fixture.undo.undoText();

    // The window's Edit menu is about to offer to undo something the user did
    // not do, so the label says where it came from.
    CHECK(local != remote);
    CHECK(remote.find("remote") != std::string::npos);
}
