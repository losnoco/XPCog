// Playback-order tests. These are the port's contract with Cog's
// PlaylistController: repeat, shuffle, the queue and what happens when the
// playing entry is deleted out from under playback.

#include "xpcog/core/FilePath.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

PlaylistEntry makeEntry(std::string title, std::string album = {}, int track = 0) {
    PlaylistEntry entry;
    entry.url      = Url::fromLocalPath(pathFromUtf8("/music/" + title + ".flac"));
    entry.rawTitle = std::move(title);
    entry.album    = std::move(album);
    entry.track    = track;
    entry.properties.format.sampleRate = 44100.0;
    entry.properties.totalFrames       = 44100;
    return entry;
}

/// Fills a playlist with `count` entries titled "1", "2", ... and returns ids.
std::vector<TrackId> fill(Playlist& playlist, int count) {
    std::vector<PlaylistEntry> entries;
    for (int i = 1; i <= count; ++i) {
        entries.push_back(makeEntry(std::to_string(i)));
    }
    return playlist.insert(playlist.size(), std::move(entries));
}

/// The titles playback visits, starting from the current entry.
std::vector<std::string> walk(Playlist& playlist, int steps) {
    std::vector<std::string> visited;
    for (int i = 0; i < steps; ++i) {
        const auto id = playlist.nextForPlayback();
        if (!id) {
            break;
        }
        playlist.setCurrent(*id);
        visited.push_back(playlist.find(*id)->rawTitle);
    }
    return visited;
}

}  // namespace

TEST_CASE("entries carry stable ids across edits", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 4);

    REQUIRE(playlist.size() == 4);
    REQUIRE(playlist.indexOf(ids[2]) == 2);

    playlist.removeAt(0);
    // The id did not change; only where it sits did. Cog identifies entries by
    // index, which is exactly why deleting a row while it plays needs special
    // handling there.
    REQUIRE(playlist.indexOf(ids[2]) == 1);
    REQUIRE(playlist.find(ids[0]) == nullptr);
}

TEST_CASE("move rotates a run of rows", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 5);  // 1 2 3 4 5

    playlist.move(0, 2, 3);  // move "1 2" so they begin at index 3
    REQUIRE(playlist.at(0).rawTitle == "3");
    REQUIRE(playlist.at(3).rawTitle == "1");
    REQUIRE(playlist.at(4).rawTitle == "2");

    playlist.move(4, 1, 0);
    REQUIRE(playlist.at(0).rawTitle == "2");
    REQUIRE(playlist.indexOf(ids[1]) == 0);
}

TEST_CASE("the read-ahead cursor advances without waiting to be heard", "[playlist]") {
    // What a gapless engine does: it asks for the next track when it stops
    // *decoding* the current one, which is a buffer's worth of audio before that
    // track is heard. So several asks can land before anything reports back
    // about what became audible, and each has to answer with a different track.
    // Measuring from the current entry answers with the track already being
    // decoded, and the engine opens and plays it a second time.
    Playlist   playlist;
    const auto ids = fill(playlist, 4);
    playlist.setCurrent(ids[0]);

    CHECK(playlist.nextForPlayback() == ids[1]);
    CHECK(playlist.nextForPlayback() == ids[2]);
    CHECK(playlist.nextForPlayback() == ids[3]);
}

TEST_CASE("what became audible does not drag the cursor back", "[playlist]") {
    // setAudible is the report that a seam reached the speaker. It moves what is
    // current -- the interface has to follow the music -- without disturbing how
    // far ahead the decoder has been allowed to run.
    Playlist   playlist;
    const auto ids = fill(playlist, 4);
    playlist.setCurrent(ids[0]);

    REQUIRE(playlist.nextForPlayback() == ids[1]);
    REQUIRE(playlist.nextForPlayback() == ids[2]);

    playlist.setAudible(ids[1]);
    CHECK(playlist.current() == ids[1]);
    CHECK(playlist.nextForPlayback() == ids[3]);
}

TEST_CASE("repositioning playback restarts the cursor", "[playlist]") {
    // setCurrent is the command, and the distinction matters: picking a track or
    // pressing Next invalidates whatever was queued up for decoding, so the next
    // answer has to come from the new position rather than from wherever the
    // read-ahead had got to.
    Playlist   playlist;
    const auto ids = fill(playlist, 4);
    playlist.setCurrent(ids[0]);

    REQUIRE(playlist.nextForPlayback() == ids[1]);
    REQUIRE(playlist.nextForPlayback() == ids[2]);

    playlist.setCurrent(ids[0]);
    CHECK(playlist.nextForPlayback() == ids[1]);
}

TEST_CASE("deleting a track the cursor has passed plays its replacement",
          "[playlist]") {
    // The engine was handed "3" and is decoding it when the user deletes it.
    // What follows must be "4" -- the entry that moves up into its place -- and
    // not "2" over again, which is what falling back to the audible entry gives.
    Playlist   playlist;
    const auto ids = fill(playlist, 5);
    playlist.setCurrent(ids[0]);

    REQUIRE(playlist.nextForPlayback() == ids[1]);
    REQUIRE(playlist.nextForPlayback() == ids[2]);

    playlist.removeAt(2, 1);  // "3", the one the cursor is sitting on
    CHECK(playlist.nextForPlayback() == ids[3]);
}

TEST_CASE("repeat none stops at the end", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::None);
    playlist.setCurrent(ids[0]);

    REQUIRE(walk(playlist, 5) == std::vector<std::string>{"2", "3"});
    REQUIRE_FALSE(playlist.nextForPlayback().has_value());
}

TEST_CASE("repeat all wraps", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::All);
    playlist.setCurrent(ids[0]);

    REQUIRE(walk(playlist, 5) == std::vector<std::string>{"2", "3", "1", "2", "3"});
}

TEST_CASE("repeat one replays until the user skips", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::One);
    playlist.setCurrent(ids[0]);

    REQUIRE(walk(playlist, 3) == std::vector<std::string>{"1", "1", "1"});

    // Next must still move. If repeat-one applied to the button too, the button
    // would appear broken -- which is why Cog passes ignoreRepeatOne:YES there.
    REQUIRE(playlist.next());
    REQUIRE(playlist.find(*playlist.current())->rawTitle == "2");
}

TEST_CASE("peeking ahead walks without moving the current entry", "[playlist]") {
    // What the quiet search for a playable track needs: look several entries
    // forward while the one being *heard* stays current, so the selection never
    // follows the search onto rows the audio does not reach.
    Playlist   playlist;
    const auto ids = fill(playlist, 4);
    playlist.setRepeat(RepeatMode::None);
    playlist.setCurrent(ids[0]);

    TrackId at = ids[0];
    std::vector<std::string> seen;
    while (const auto ahead = playlist.peekNext(at)) {
        seen.push_back(playlist.find(*ahead)->rawTitle);
        at = *ahead;
    }
    CHECK(seen == std::vector<std::string>{"2", "3", "4"});
    CHECK(playlist.current() == ids[0]);
}

TEST_CASE("peeking ahead ignores repeat-one, as Next does", "[playlist]") {
    // Otherwise the search would offer the track already playing for ever, and a
    // playlist on repeat-one could never be searched at all.
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::One);
    playlist.setCurrent(ids[0]);

    const auto ahead = playlist.peekNext(ids[0]);
    REQUIRE(ahead.has_value());
    CHECK(playlist.find(*ahead)->rawTitle == "2");
    CHECK(playlist.current() == ids[0]);
}

TEST_CASE("peeking back walks without moving the current entry", "[playlist]") {
    // Previous searches too, and it needs the same footing Next has: walk
    // backwards from each answer while the entry being heard stays current.
    Playlist   playlist;
    const auto ids = fill(playlist, 4);
    playlist.setRepeat(RepeatMode::None);
    playlist.setCurrent(ids[3]);

    TrackId                  at = ids[3];
    std::vector<std::string> seen;
    while (const auto back = playlist.peekPrevious(at)) {
        seen.push_back(playlist.find(*back)->rawTitle);
        at = *back;
    }
    CHECK(seen == std::vector<std::string>{"3", "2", "1"});
    CHECK(playlist.current() == ids[3]);
}

TEST_CASE("peeking back ignores repeat-one, as Previous does", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::One);
    playlist.setCurrent(ids[2]);

    const auto back = playlist.peekPrevious(ids[2]);
    REQUIRE(back.has_value());
    CHECK(playlist.find(*back)->rawTitle == "2");
    CHECK(playlist.current() == ids[2]);
}

TEST_CASE("repeat album loops the album, not the playlist", "[playlist]") {
    Playlist                   playlist;
    std::vector<PlaylistEntry> entries;
    entries.push_back(makeEntry("a1", "A", 1));
    entries.push_back(makeEntry("a2", "A", 2));
    entries.push_back(makeEntry("b1", "B", 1));
    entries.push_back(makeEntry("b2", "B", 2));
    const auto ids = playlist.insert(0, std::move(entries));

    playlist.setRepeat(RepeatMode::Album);
    playlist.setCurrent(ids[0]);

    REQUIRE(walk(playlist, 5) ==
            std::vector<std::string>{"a2", "a1", "a2", "a1", "a2"});

    // Starting inside the second album loops that one instead.
    playlist.setCurrent(ids[3]);
    REQUIRE(walk(playlist, 2) == std::vector<std::string>{"b1", "b2"});
}

TEST_CASE("repeat album matches album names case-insensitively", "[playlist]") {
    Playlist                   playlist;
    std::vector<PlaylistEntry> entries;
    entries.push_back(makeEntry("one", "Wish You Were Here", 1));
    entries.push_back(makeEntry("two", "WISH YOU WERE HERE", 2));
    const auto ids = playlist.insert(0, std::move(entries));

    playlist.setRepeat(RepeatMode::Album);
    playlist.setCurrent(ids[0]);
    REQUIRE(walk(playlist, 3) == std::vector<std::string>{"two", "one", "two"});
}

TEST_CASE("the queue outranks the ordering mode", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 5);
    playlist.setCurrent(ids[0]);

    playlist.enqueue(ids[4]);
    playlist.enqueue(ids[2]);
    REQUIRE(playlist.find(ids[4])->queuePosition == 0);
    REQUIRE(playlist.find(ids[2])->queuePosition == 1);

    REQUIRE(walk(playlist, 3) == std::vector<std::string>{"5", "3", "4"});
    //                                                      ^ queue drained, then
    //                                                        linear from "3"
    REQUIRE(playlist.queue().empty());
    REQUIRE(playlist.find(ids[4])->queuePosition == -1);
}

TEST_CASE("removing a queued entry renumbers the rest", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 4);

    playlist.enqueue(ids[1]);
    playlist.enqueue(ids[2]);
    playlist.enqueue(ids[3]);
    playlist.remove({ids[2]});

    REQUIRE(playlist.queue() == std::vector<TrackId>{ids[1], ids[3]});
    REQUIRE(playlist.find(ids[3])->queuePosition == 1);
}

TEST_CASE("deleting the playing entry resumes where it was", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 5);
    playlist.setRepeat(RepeatMode::None);
    playlist.setCurrent(ids[2]);  // "3"

    playlist.removeAt(2);  // the playing entry goes away

    // "4" moved into slot 2. Playback continues there rather than restarting at
    // the top of the playlist, which is what an index-based model does.
    const auto id = playlist.nextForPlayback();
    REQUIRE(id.has_value());
    REQUIRE(playlist.find(*id)->rawTitle == "4");
}

TEST_CASE("deleting the last entry while it plays ends playback", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::None);
    playlist.setCurrent(ids[2]);

    playlist.removeAt(2);
    REQUIRE_FALSE(playlist.nextForPlayback().has_value());
}

TEST_CASE("shuffle visits every track exactly once per pass", "[playlist]") {
    Playlist playlist;
    playlist.seedShuffle(20260816);
    const auto ids = fill(playlist, 12);

    playlist.setRepeat(RepeatMode::All);
    playlist.setCurrent(ids[0]);
    playlist.setShuffle(ShuffleMode::All);

    // The playing track heads the order, so the first pass is the current track
    // plus the other eleven.
    std::vector<std::string> pass{"1"};
    for (const std::string& title : walk(playlist, 11)) {
        pass.push_back(title);
    }

    const std::set<std::string> unique(pass.begin(), pass.end());
    REQUIRE(unique.size() == 12);

    // A permutation, not the identity -- otherwise the test would pass with
    // shuffle silently doing nothing.
    std::vector<std::string> ordered;
    for (int i = 1; i <= 12; ++i) {
        ordered.push_back(std::to_string(i));
    }
    REQUIRE(pass != ordered);
}

TEST_CASE("shuffle keeps playing across the pass boundary", "[playlist]") {
    Playlist playlist;
    playlist.seedShuffle(7);
    const auto ids = fill(playlist, 4);

    playlist.setRepeat(RepeatMode::All);
    playlist.setCurrent(ids[0]);
    playlist.setShuffle(ShuffleMode::All);

    REQUIRE(walk(playlist, 11).size() == 11);
}

TEST_CASE("shuffle stops at the end of a pass without repeat all", "[playlist]") {
    Playlist playlist;
    playlist.seedShuffle(7);
    const auto ids = fill(playlist, 4);

    playlist.setCurrent(ids[0]);
    playlist.setShuffle(ShuffleMode::All);
    playlist.setRepeat(RepeatMode::None);

    REQUIRE(walk(playlist, 10).size() == 3);
}

TEST_CASE("turning shuffle on does not change what is playing", "[playlist]") {
    Playlist playlist;
    playlist.seedShuffle(99);
    const auto ids = fill(playlist, 20);
    playlist.setCurrent(ids[7]);

    playlist.setShuffle(ShuffleMode::All);

    REQUIRE(playlist.current() == ids[7]);
    REQUIRE(playlist.find(ids[7])->shuffleIndex == 0);
}

TEST_CASE("album shuffle keeps albums whole and in running order", "[playlist]") {
    Playlist playlist;
    playlist.seedShuffle(4242);

    std::vector<PlaylistEntry> entries;
    for (const std::string& album : {std::string{"A"}, std::string{"B"},
                                     std::string{"C"}}) {
        // Deliberately inserted out of track order.
        entries.push_back(makeEntry(album + "3", album, 3));
        entries.push_back(makeEntry(album + "1", album, 1));
        entries.push_back(makeEntry(album + "2", album, 2));
    }
    playlist.insert(0, std::move(entries));

    playlist.setRepeat(RepeatMode::All);
    playlist.setShuffle(ShuffleMode::Albums);

    std::vector<std::string> order;
    const auto               first = playlist.nextForPlayback();
    REQUIRE(first.has_value());
    playlist.setCurrent(*first);
    order.push_back(playlist.find(*first)->rawTitle);
    for (const std::string& title : walk(playlist, 8)) {
        order.push_back(title);
    }

    REQUIRE(order.size() == 9);
    for (std::size_t i = 0; i < 9; i += 3) {
        const std::string album = order[i].substr(0, 1);
        REQUIRE(order[i] == album + "1");
        REQUIRE(order[i + 1] == album + "2");
        REQUIRE(order[i + 2] == album + "3");
    }
}

TEST_CASE("previous walks back and wraps under repeat all", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setRepeat(RepeatMode::All);
    playlist.setCurrent(ids[0]);

    REQUIRE(playlist.previous());
    REQUIRE(playlist.find(*playlist.current())->rawTitle == "3");
    REQUIRE(playlist.previous());
    REQUIRE(playlist.find(*playlist.current())->rawTitle == "2");
}

TEST_CASE("stop after current halts playback but not the buttons", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setCurrent(ids[0]);
    playlist.setStopAfterCurrent(true);

    REQUIRE_FALSE(playlist.nextForPlayback().has_value());
    REQUIRE(playlist.next());
    REQUIRE(playlist.find(*playlist.current())->rawTitle == "2");
}

TEST_CASE("stop after one track halts playback only after that track",
          "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setCurrent(ids[0]);
    playlist.update(ids[1], [](PlaylistEntry& entry) { entry.stopAfter = true; });

    // Track 1 has nothing set, so it hands out track 2 as usual.
    REQUIRE(playlist.nextForPlayback() == ids[1]);
    // Track 2 is the one that was marked, and playback ends there.
    REQUIRE_FALSE(playlist.nextForPlayback().has_value());
}

TEST_CASE("stop after a track does not stop the Next button", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setCurrent(ids[0]);
    playlist.update(ids[0], [](PlaylistEntry& entry) { entry.stopAfter = true; });

    REQUIRE_FALSE(playlist.nextForPlayback().has_value());
    // Cog checks the flag as the stream ends and nowhere else. A Next that did
    // nothing because a flag was set earlier is a broken button.
    REQUIRE(playlist.next());
    REQUIRE(playlist.find(*playlist.current())->rawTitle == "2");
}

TEST_CASE("leaving a track clears its stop-after mark", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);
    playlist.setCurrent(ids[0]);
    playlist.update(ids[0], [](PlaylistEntry& entry) { entry.stopAfter = true; });

    playlist.setCurrent(ids[1]);
    REQUIRE_FALSE(playlist.find(ids[0])->stopAfter);

    // And the first track plays through the next time round rather than
    // stopping again for a reason nobody can see. Cog clears it in
    // -setCurrentEntry: for exactly this.
    playlist.setCurrent(ids[0]);
    REQUIRE(playlist.nextForPlayback() == ids[1]);
}

TEST_CASE("observers see structural changes", "[playlist]") {
    Playlist                       playlist;
    std::vector<Playlist::Change>  seen;
    const Subscription             token =
        playlist.observe([&seen](const Playlist::Change& change) { seen.push_back(change); });

    const auto ids = fill(playlist, 3);
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].kind == Playlist::Change::Kind::Inserted);
    REQUIRE(seen[0].count == 3);

    playlist.setCurrent(ids[1]);
    REQUIRE(seen.back().kind == Playlist::Change::Kind::Current);

    playlist.removeAt(0);
    REQUIRE(seen.back().kind == Playlist::Change::Kind::Removed);
}

TEST_CASE("dropping the subscription disconnects", "[playlist]") {
    Playlist playlist;
    int      calls = 0;
    {
        const Subscription token =
            playlist.observe([&calls](const Playlist::Change&) { ++calls; });
        fill(playlist, 1);
    }
    fill(playlist, 1);
    REQUIRE(calls == 1);
}

TEST_CASE("a signal outliving its subscription does not crash", "[playlist]") {
    Subscription token;
    {
        Signal<int> signal;
        token = signal.connect([](const int&) {});
        REQUIRE(token.connected());
    }
    REQUIRE_FALSE(token.connected());
    token.reset();  // must be a no-op, not a use-after-free
}

TEST_CASE("metadata promotes the tags playback sorts on", "[playlist]") {
    MetadataMap tags;
    tags.set("album", "Animals");
    tags.set("album artist", "Pink Floyd");
    tags.add("artist", "Pink Floyd");
    tags.set("title", "Dogs");
    tags.set("tracknumber", "2/5");
    tags.set("date", "1977-01-23");
    tags.set("mood", "bleak");

    PlaylistEntry entry;
    entry.url = Url::fromLocalPath("/music/dogs.flac");
    entry.applyMetadata(tags);

    REQUIRE(entry.album == "Animals");
    REQUIRE(entry.albumArtist == "Pink Floyd");  // via the "album artist" alias
    REQUIRE(entry.track == 2);                   // "2/5" stops at the slash
    REQUIRE(entry.year == 1977);                 // derived from date
    REQUIRE(entry.display() == "Pink Floyd - Dogs");

    // Promoted tags leave the map, so a writer cannot emit them twice.
    REQUIRE(entry.metadata.find("album") == nullptr);
    REQUIRE(entry.metadata.first("mood") == "bleak");
}

TEST_CASE("an untitled entry falls back to its file name", "[playlist]") {
    PlaylistEntry entry;
    entry.url = Url::fromLocalPath("/music/Pink Floyd - Animals.flac")
                    .withFragment("2");

    REQUIRE(entry.title() == "Pink Floyd - Animals.flac#2");
    REQUIRE(entry.display() == "Pink Floyd - Animals.flac#2");
}

// --- reinsert and reorder, which exist for the app's undo stack -----------

TEST_CASE("reinsert brings entries back with the ids they had", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 4);

    std::vector<PlaylistEntry> taken{playlist.at(1), playlist.at(2)};
    playlist.removeAt(1, 2);
    REQUIRE(playlist.size() == 2);

    playlist.reinsert(1, taken);
    REQUIRE(playlist.size() == 4);
    REQUIRE(playlist.at(1).id == ids[1]);
    REQUIRE(playlist.at(2).id == ids[2]);
    REQUIRE(playlist.indexOf(ids[2]) == 2u);

    // The id counter has to stay ahead of what was restored, or the next add
    // hands out an id that is already in the list.
    const TrackId fresh = playlist.add(makeEntry("new"));
    for (const TrackId existing : ids) {
        REQUIRE(fresh != existing);
    }
}

TEST_CASE("a reinserted entry does not claim a place in the queue", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);

    playlist.enqueue(ids[1]);
    REQUIRE(playlist.at(1).queued());

    std::vector<PlaylistEntry> taken{playlist.at(1)};
    playlist.removeAt(1, 1);
    playlist.reinsert(1, taken);

    // The entry carried queuePosition 0 when it was captured, but reinsert does
    // not restore the queue, so it must not come back looking queued.
    REQUIRE_FALSE(playlist.at(1).queued());
    REQUIRE(playlist.queue().empty());

    // ...and it must be possible to put it back in the queue, which is what
    // enqueue() refuses to do for something already queued.
    playlist.enqueue(ids[1]);
    REQUIRE(playlist.queue() == std::vector<TrackId>{ids[1]});
}

TEST_CASE("reorder rearranges into exactly the order given", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 4);

    REQUIRE(playlist.reorder({ids[3], ids[0], ids[2], ids[1]}));
    REQUIRE(playlist.at(0).id == ids[3]);
    REQUIRE(playlist.at(3).id == ids[1]);
    REQUIRE(playlist.indexOf(ids[0]) == 1u);
}

TEST_CASE("reorder refuses anything that is not a permutation", "[playlist]") {
    Playlist   playlist;
    const auto ids = fill(playlist, 3);

    // Too short, an unknown id, and a duplicate that would silently drop an
    // entry while still having the right length.
    REQUIRE_FALSE(playlist.reorder({ids[0], ids[1]}));
    REQUIRE_FALSE(playlist.reorder({ids[0], ids[1], 999}));
    REQUIRE_FALSE(playlist.reorder({ids[0], ids[1], ids[1]}));

    // Nothing changed.
    REQUIRE(playlist.at(0).id == ids[0]);
    REQUIRE(playlist.at(1).id == ids[1]);
    REQUIRE(playlist.at(2).id == ids[2]);
}
