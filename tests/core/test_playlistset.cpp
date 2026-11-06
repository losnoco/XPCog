// Several playlists: what they share and what they keep to themselves.

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/library/Library.hpp"
#include "xpcog/core/library/PlaylistSet.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

// Private to core; only the upgrade test reaches for it, to write an old store.
#include "../../core/src/library/Sqlite.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

PlaylistEntry track(const std::string& name) {
    PlaylistEntry entry;
    entry.url = Url::fromLocalPath(pathFromUtf8("/music/" + name + ".flac"));
    return entry;
}

std::vector<TrackId> fill(Playlist& list, const std::string& prefix, int count) {
    std::vector<TrackId> ids;
    for (int i = 0; i < count; ++i) {
        ids.push_back(list.add(track(prefix + std::to_string(i))));
    }
    return ids;
}

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("a new set is one empty playlist, shown and playing", "[playlist]") {
    PlaylistSet set;
    REQUIRE(set.size() == 1);
    CHECK(set.shownId() == PlaylistSet::kFirstId);
    CHECK(set.playingId() == PlaylistSet::kFirstId);
    CHECK(set.shown().empty());
}

TEST_CASE("ids are unique across the playlists of a set", "[playlist]") {
    PlaylistSet set;
    const PlaylistSet::Id second = set.add("second");

    std::set<TrackId> seen;
    for (const TrackId id : fill(set.shown(), "a", 3)) {
        CHECK(seen.insert(id).second);
    }
    for (const TrackId id : fill(*set.playlist(second), "b", 3)) {
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("looking at another playlist does not change what plays next", "[playlist]") {
    PlaylistSet set;
    const std::vector<TrackId> first = fill(set.playing(), "a", 3);
    const PlaylistSet::Id      other = set.add("other");
    fill(*set.playlist(other), "b", 3);

    set.setCurrent(first[0]);
    set.show(other);

    CHECK(set.playingId() == PlaylistSet::kFirstId);
    CHECK(set.nextForPlayback() == first[1]);
}

TEST_CASE("the last playlist cannot be removed", "[playlist]") {
    PlaylistSet set;
    CHECK_FALSE(set.remove(PlaylistSet::kFirstId));
    CHECK(set.size() == 1);
}

TEST_CASE("removing the shown and playing playlist moves both to its neighbour", "[playlist]") {
    PlaylistSet           set;
    const PlaylistSet::Id second = set.add("second");
    const PlaylistSet::Id third  = set.add("third");
    set.show(second);
    set.setPlaying(second);

    REQUIRE(set.remove(second));
    CHECK(set.shownId() == third);
    CHECK(set.playingId() == third);

    REQUIRE(set.remove(third));
    CHECK(set.shownId() == PlaylistSet::kFirstId);
}

TEST_CASE("a removed id is not given out again", "[playlist]") {
    PlaylistSet           set;
    const PlaylistSet::Id second = set.add("second");
    REQUIRE(set.remove(second));
    CHECK(set.add("third") != second);
}

TEST_CASE("playing another playlist leaves no playing row behind", "[playlist]") {
    PlaylistSet                set;
    const std::vector<TrackId> first  = fill(set.playing(), "a", 2);
    const PlaylistSet::Id      other  = set.add("other");
    const std::vector<TrackId> second = fill(*set.playlist(other), "b", 2);

    set.setCurrent(first[0]);
    set.setCurrent(second[1]);

    CHECK(set.playingId() == other);
    CHECK_FALSE(set.playlist(PlaylistSet::kFirstId)->current());
    CHECK(set.playing().current() == second[1]);
}

TEST_CASE("the queue is one across playlists, and numbered across them", "[playlist]") {
    PlaylistSet                set;
    const std::vector<TrackId> first  = fill(set.playing(), "a", 3);
    const PlaylistSet::Id      other  = set.add("other");
    Playlist&                  list   = *set.playlist(other);
    const std::vector<TrackId> second = fill(list, "b", 3);

    set.playing().enqueue(first[2]);
    list.enqueue(second[1]);

    CHECK(set.playing().queue().size() == 2);
    CHECK(list.queue().size() == 2);
    CHECK(set.find(first[2])->queuePosition == 0);
    CHECK(set.find(second[1])->queuePosition == 1);

    // Taking the first out renumbers the entry in the other playlist.
    set.playing().dequeue(first[2]);
    CHECK(set.find(first[2])->queuePosition == -1);
    CHECK(set.find(second[1])->queuePosition == 0);
}

TEST_CASE("a queued track from another playlist carries playback across", "[playlist]") {
    PlaylistSet                set;
    const std::vector<TrackId> first  = fill(set.playing(), "a", 3);
    const PlaylistSet::Id      other  = set.add("other");
    Playlist&                  list   = *set.playlist(other);
    const std::vector<TrackId> second = fill(list, "b", 3);

    set.setCurrent(first[0]);
    list.enqueue(second[1]);

    SECTION("at a seam, keeping the audible row until the audio arrives") {
        CHECK(set.nextForPlayback() == second[1]);
        CHECK(set.playingId() == other);
        CHECK(set.playlist(PlaylistSet::kFirstId)->current() == first[0]);

        // What follows is measured from where the queue left playback.
        CHECK(set.nextForPlayback() == second[2]);

        set.noteAudible(second[1]);
        CHECK_FALSE(set.playlist(PlaylistSet::kFirstId)->current());
        CHECK(list.current() == second[1]);
    }

    SECTION("by the Next button") {
        REQUIRE(set.next());
        CHECK(set.playingId() == other);
        CHECK(list.current() == second[1]);
        CHECK(list.queue().empty());
    }
}

TEST_CASE("clearing a playlist takes only its own tracks out of the queue", "[playlist]") {
    PlaylistSet                set;
    const std::vector<TrackId> first  = fill(set.playing(), "a", 2);
    const PlaylistSet::Id      other  = set.add("other");
    Playlist&                  list   = *set.playlist(other);
    const std::vector<TrackId> second = fill(list, "b", 2);

    set.playing().enqueue(first[0]);
    list.enqueue(second[0]);

    set.playing().clear();

    REQUIRE(list.queue().size() == 1);
    CHECK(list.queue().front() == second[0]);
    CHECK(set.find(second[0])->queuePosition == 0);
}

TEST_CASE("removing a playlist drops what it had queued", "[playlist]") {
    PlaylistSet                set;
    const PlaylistSet::Id      other  = set.add("other");
    const std::vector<TrackId> second = fill(*set.playlist(other), "b", 2);
    set.playlist(other)->enqueue(second[0]);

    REQUIRE(set.remove(other));
    CHECK(set.playing().queue().empty());
}

TEST_CASE("findByUrl prefers the playing playlist", "[playlist]") {
    PlaylistSet           set;
    const TrackId         shared = set.playing().add(track("same"));
    const PlaylistSet::Id other  = set.add("other");
    const TrackId         copy   = set.playlist(other)->add(track("same"));

    const std::string url = set.find(shared)->url.toString();
    CHECK(set.findByUrl(url) == shared);

    set.setPlaying(other);
    CHECK(set.findByUrl(url) == copy);
}

TEST_CASE("a view can be pointed at another playlist", "[playlist]") {
    PlaylistSet           set;
    fill(set.shown(), "a", 2);
    const PlaylistSet::Id other = set.add("other");
    fill(*set.playlist(other), "b", 5);

    PlaylistView view{set.shown()};
    REQUIRE(view.rowCount() == 2);

    int rebuilds = 0;
    const Subscription watch = view.rebuilt.connect([&rebuilds] { ++rebuilds; });
    view.setPlaylist(*set.playlist(other));

    CHECK(view.rowCount() == 5);
    CHECK(rebuilds == 1);

    // And follows it, not the one it left.
    set.playlist(other)->add(track("c"));
    CHECK(view.rowCount() == 6);
    set.playlist(PlaylistSet::kFirstId)->add(track("d"));
    CHECK(view.rowCount() == 6);
}

TEST_CASE("playlists survive the library", "[playlist][library]") {
    TempFile file{"xpcog-playlistset-roundtrip.db"};

    TrackId queuedHere  = 0;
    TrackId queuedThere = 0;
    TrackId playing     = 0;
    PlaylistSet::Id other = 0;
    {
        Library library;
        REQUIRE(library.open(file.path));

        PlaylistSet set;
        const std::vector<TrackId> first = fill(set.playing(), "a", 3);
        other                            = set.add("other");
        Playlist&                  list  = *set.playlist(other);
        const std::vector<TrackId> second = fill(list, "b", 2);

        queuedThere = second[1];
        queuedHere  = first[2];
        playing     = second[0];
        list.enqueue(queuedThere);
        set.playing().enqueue(queuedHere);
        set.setCurrent(playing);

        REQUIRE(library.savePlaylists(set));
    }

    Library library;
    REQUIRE(library.open(file.path));
    PlaylistSet loaded;
    REQUIRE(library.loadPlaylists(loaded));

    REQUIRE(loaded.size() == 2);
    CHECK(loaded.at(0).id == PlaylistSet::kFirstId);
    CHECK(loaded.at(0).name.empty());
    CHECK(loaded.at(1).name == "other");
    CHECK(loaded.at(0).playlist->size() == 3);
    CHECK(loaded.at(1).playlist->size() == 2);

    // Playing and queue both as they were, the queue in its order across both.
    CHECK(loaded.playingId() == other);
    CHECK(loaded.playing().current() == playing);
    REQUIRE(loaded.playing().queue().size() == 2);
    CHECK(loaded.playing().queue()[0] == queuedThere);
    CHECK(loaded.playing().queue()[1] == queuedHere);

    // A new track must not take an id a stored one has, in either playlist.
    const TrackId fresh = loaded.shown().add(track("new"));
    CHECK(loaded.find(fresh) != nullptr);
    CHECK(fresh > queuedHere);
    CHECK(fresh > queuedThere);
}

TEST_CASE("a removed playlist's entries leave the library", "[playlist][library]") {
    TempFile file{"xpcog-playlistset-removed.db"};
    Library  library;
    REQUIRE(library.open(file.path));

    PlaylistSet set;
    fill(set.playing(), "a", 2);
    const PlaylistSet::Id other = set.add("other");
    fill(*set.playlist(other), "b", 2);
    REQUIRE(library.savePlaylists(set));

    REQUIRE(set.remove(other));
    REQUIRE(library.savePlaylists(set));

    PlaylistSet loaded;
    REQUIRE(library.loadPlaylists(loaded));
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.shown().size() == 2);
}

TEST_CASE("a library from before there were several reads as one playlist",
          "[playlist][library]") {
    TempFile file{"xpcog-playlistset-upgrade.db"};
    {
        Library library;
        REQUIRE(library.open(file.path));
        Playlist list;
        fill(list, "a", 4);
        REQUIRE(library.savePlaylist(list));
    }

    // Back to what version 4 left: no table, no column.
    {
        sql::Database database;
        REQUIRE(database.open(file.path));
        REQUIRE(database.exec("DROP TABLE playlist;"));
        REQUIRE(database.exec("ALTER TABLE playlist_entry DROP COLUMN playlist_id;"));
        REQUIRE(database.exec("UPDATE schema_version SET version = 4;"));
    }

    Library library;
    REQUIRE(library.open(file.path));
    CHECK(library.schemaVersion() == 5);

    PlaylistSet loaded;
    REQUIRE(library.loadPlaylists(loaded));
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.at(0).id == PlaylistSet::kFirstId);
    CHECK(loaded.at(0).name.empty());
    CHECK(loaded.shown().size() == 4);
}
