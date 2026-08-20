// The playlist's sort and filter.
//
// These used to need Qt, because the ordering lived in a QSortFilterProxyModel.
// They do not any more, and that is most of the reason PlaylistView exists: the
// bug they were written for is the one nobody reports precisely -- "filtering
// messes up the order" -- and it is worth having it pinned by a test that runs
// everywhere rather than only where a display does.
//
// The removal cases the old suite carried are gone rather than ported, and that
// is the right outcome: they existed because Playlist notifies *after* it has
// changed while Qt's model protocol wanted the opposite, and PlaylistModel had a
// hand-maintained row count bridging the two. `ASSERT: "last < rowCount(parent)"`
// on select-all-and-delete was the symptom. PlaylistView reads the playlist live
// and rebuilds, so there is no second count to keep in step and nothing to assert
// about. The row-count-through-every-edit case survives below, because that one
// is about this class rather than about the bridge.

#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace xpcog;

namespace {

using Column = PlaylistView::Column;

/// Ten tracks that all share an artist and an album, so every sort key but the
/// title is a tie. Real playlists look exactly like this -- an album is ten rows
/// with one artist -- which is why ties are the common case and not the edge.
Playlist makeAlbum() {
    Playlist                   playlist;
    std::vector<PlaylistEntry> entries;
    for (int i = 1; i <= 10; ++i) {
        PlaylistEntry entry;
        entry.url = *Url::parse("file:///music/track" + std::to_string(i) + ".flac");
        entry.artist   = "One Artist";
        entry.album    = "One Album";
        entry.rawTitle = "Track " + std::to_string(i);
        entry.track    = i;
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));
    return playlist;
}

/// The titles as the view would show them, top to bottom.
std::vector<std::string> visibleTitles(const PlaylistView& view) {
    std::vector<std::string> titles;
    for (std::size_t row = 0; row < view.rowCount(); ++row) {
        titles.push_back(view.text(row, Column::Title));
    }
    return titles;
}

std::vector<std::string> albumOrder() {
    std::vector<std::string> titles;
    for (int i = 1; i <= 10; ++i) {
        titles.push_back("Track " + std::to_string(i));
    }
    return titles;
}

}  // namespace

TEST_CASE("an unsorted view shows the playlist in playlist order", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    CHECK(view.rowCount() == 10);
    CHECK(visibleTitles(view) == albumOrder());
}

TEST_CASE("filtering does not reorder rows that tie on the sort key",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    // Sorting by artist when every artist is the same: the comparison can
    // distinguish nothing, so the order has to come from somewhere else. If it
    // came from wherever a binary search happened to land, the rows would move
    // about whenever the filter changed.
    view.setSort(Column::Artist, true);
    REQUIRE(visibleTitles(view) == albumOrder());

    view.setFilter("Track 1");  // Track 1 and Track 10
    CHECK(view.rowCount() == 2);

    view.setFilter("");
    CHECK(visibleTitles(view) == albumOrder());
}

TEST_CASE("a filter applied and cleared repeatedly leaves the order untouched",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};
    view.setSort(Column::Album, true);  // every row ties

    for (int round = 0; round < 100; ++round) {
        view.setFilter("Track");
        view.setFilter("");
    }

    // True by construction with a stable sort over a rebuilt mapping, which is
    // exactly why it is worth asserting: it is the property that broke when the
    // ordering was maintained incrementally instead.
    CHECK(visibleTitles(view) == albumOrder());
}

TEST_CASE("sorting by a column with real values still works", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    view.setSort(Column::Track, false);

    std::vector<std::string> reversed = albumOrder();
    std::reverse(reversed.begin(), reversed.end());
    CHECK(visibleTitles(view) == reversed);
}

TEST_CASE("track numbers sort numerically, not as text", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    view.setSort(Column::Track, true);

    // Lexicographically "10" precedes "2". Track 10 must still come last.
    CHECK(visibleTitles(view).back() == "Track 10");
}

TEST_CASE("titles sort naturally, so Track 9 precedes Track 10", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    view.setSort(Column::Title, true);

    // The same assertion as the track-number case, reached a different way: this
    // one goes through naturalLess rather than through a numeric compare, and it
    // is the reason core already had that comparator.
    CHECK(visibleTitles(view).back() == "Track 10");
}

TEST_CASE("the filter matches title, artist and album, case-insensitively",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    view.setFilter("one artist");
    CHECK(view.rowCount() == 10);

    view.setFilter("ONE ALBUM");
    CHECK(view.rowCount() == 10);

    view.setFilter("track 7");
    CHECK(view.rowCount() == 1);

    view.setFilter("nothing here");
    CHECK(view.rowCount() == 0);
}

TEST_CASE("the row count tracks the playlist through every edit", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    REQUIRE(view.rowCount() == playlist.size());

    playlist.removeAt(9, 1);
    CHECK(view.rowCount() == playlist.size());

    playlist.removeAt(0, 3);
    CHECK(view.rowCount() == playlist.size());

    std::vector<PlaylistEntry> more;
    PlaylistEntry              added;
    added.url      = *Url::parse("file:///music/added.flac");
    added.rawTitle = "Added";
    more.push_back(added);
    playlist.insert(1, std::move(more));
    CHECK(view.rowCount() == playlist.size());

    playlist.clear();
    CHECK(view.rowCount() == 0);
}

TEST_CASE("removing every row leaves an empty view", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    // The whole selection at once, which is what select-all-and-delete does and
    // what used to abort.
    playlist.remove(view.visibleTracks());

    CHECK(view.rowCount() == 0);
    CHECK(playlist.size() == 0);
}

TEST_CASE("a row is found by its track, and not when filtered away",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    const TrackId seventh = playlist.at(6).id;
    REQUIRE(view.rowForTrack(seventh) == std::size_t{6});

    view.setSort(Column::Track, false);
    CHECK(view.rowForTrack(seventh) == std::size_t{3});

    view.setFilter("Track 1");
    CHECK_FALSE(view.rowForTrack(seventh).has_value());
}

TEST_CASE("the current track is marked, and only the two affected rows are announced",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    std::vector<std::size_t> changed;
    const Subscription       subscription =
        view.rowChanged.connect([&](std::size_t row) { changed.push_back(row); });

    const TrackId third = playlist.at(2).id;
    view.setCurrentTrack(third);

    CHECK(view.text(2, Column::Status) == "\xE2\x96\xB6");
    // Only the row that gained the marker: there was no previous one.
    CHECK(changed == std::vector<std::size_t>{2});

    changed.clear();
    const TrackId fifth = playlist.at(4).id;
    view.setCurrentTrack(fifth);

    // The row that lost it and the row that gained it, and nothing else -- a
    // full redraw of a long list for two glyphs is waste.
    CHECK(changed == std::vector<std::size_t>{2, 4});
    CHECK(view.text(2, Column::Status).empty());
    CHECK(view.text(4, Column::Status) == "\xE2\x96\xB6");
}

TEST_CASE("a metadata update does not rebuild the mapping", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    int rebuilds = 0;
    int rowEdits = 0;
    const Subscription onRebuild = view.rebuilt.connect([&] { ++rebuilds; });
    const Subscription onRow     = view.rowChanged.connect([&](std::size_t) { ++rowEdits; });

    playlist.update(playlist.at(3).id,
                    [](PlaylistEntry& entry) { entry.rawTitle = "Track 4 (Remastered)"; });

    // The case that matters: a scan publishes one of these per file, and
    // resetting the whole view that often would make the list unusable while it
    // ran.
    CHECK(rebuilds == 0);
    CHECK(rowEdits == 1);
    CHECK(view.text(3, Column::Title) == "Track 4 (Remastered)");
}

TEST_CASE("an update that changes whether a row passes the filter rebuilds",
          "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};
    view.setFilter("Remastered");
    REQUIRE(view.rowCount() == 0);

    int                rebuilds  = 0;
    const Subscription onRebuild = view.rebuilt.connect([&] { ++rebuilds; });

    playlist.update(playlist.at(3).id,
                    [](PlaylistEntry& entry) { entry.rawTitle = "Track 4 (Remastered)"; });

    // The row now matches where it did not, so the cheap path would have left it
    // hidden and reported a change to a row that is not shown.
    CHECK(rebuilds == 1);
    CHECK(view.rowCount() == 1);
    CHECK(view.text(0, Column::Title) == "Track 4 (Remastered)");
}

TEST_CASE("sort order never reaches the playlist", "[core][playlist]") {
    Playlist     playlist = makeAlbum();
    PlaylistView view{playlist};

    std::vector<TrackId> before;
    for (const PlaylistEntry& entry : playlist.entries()) {
        before.push_back(entry.id);
    }

    view.setSort(Column::Track, false);
    view.setFilter("Track 1");

    std::vector<TrackId> after;
    for (const PlaylistEntry& entry : playlist.entries()) {
        after.push_back(entry.id);
    }

    // XPCog's one deliberate behaviour difference from Cog, which shuffles and
    // steps through the *sorted* order. Display-only means display-only.
    CHECK(before == after);
}
