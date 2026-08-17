// The playlist table's sort and filter.
//
// These are the tests xpcog-cli cannot carry, because they need Qt. The bug they
// exist for is the one nobody reports precisely: "filtering messes up the order".

#include "QtStringMaker.hpp"

#include "PlaylistModel.hpp"

#include "xpcog/core/library/Playlist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QSortFilterProxyModel>
#include <QStringList>

using namespace xpcog;
using namespace xpcog::app;

namespace {

/// Ten tracks that all share an artist and an album, so every sort key but the
/// title is a tie. Real playlists look exactly like this -- an album is ten rows
/// with one artist -- which is why ties are the common case and not the edge.
Playlist makeAlbum() {
    Playlist playlist;
    std::vector<PlaylistEntry> entries;
    for (int i = 1; i <= 10; ++i) {
        PlaylistEntry entry;
        entry.url = *Url::parse("file:///music/track" + std::to_string(i) + ".flac");
        entry.artist = "One Artist";
        entry.album  = "One Album";
        entry.rawTitle = "Track " + std::to_string(i);
        entry.track  = i;
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));
    return playlist;
}

/// The titles as the view would show them, top to bottom.
QStringList visibleTitles(const QAbstractItemModel& model) {
    QStringList titles;
    for (int row = 0; row < model.rowCount(); ++row) {
        titles << model.data(model.index(row, PlaylistModel::ColumnTitle)).toString();
    }
    return titles;
}

QStringList albumOrder() {
    QStringList titles;
    for (int i = 1; i <= 10; ++i) {
        titles << QStringLiteral("Track %1").arg(i);
    }
    return titles;
}

}  // namespace

TEST_CASE("an unsorted proxy shows the playlist in playlist order", "[app][playlist]") {
    Playlist           playlist = makeAlbum();
    PlaylistModel      model{playlist};
    PlaylistProxyModel proxy;
    proxy.setSourceModel(&model);

    CHECK(visibleTitles(proxy) == albumOrder());
}

TEST_CASE("filtering does not reorder rows that tie on the sort key",
          "[app][playlist]") {
    Playlist           playlist = makeAlbum();
    PlaylistModel      model{playlist};
    PlaylistProxyModel proxy;
    proxy.setSourceModel(&model);

    // Sorting by artist when every artist is the same: the comparison can
    // distinguish nothing, so the order has to come from somewhere else. If it
    // comes from wherever the proxy's binary search happens to land, the rows
    // move about whenever the filter changes.
    proxy.sort(PlaylistModel::ColumnArtist, Qt::AscendingOrder);
    REQUIRE(visibleTitles(proxy) == albumOrder());

    proxy.setFilterText(QStringLiteral("Track 1"));  // Track 1 and Track 10
    CHECK(proxy.rowCount() == 2);

    proxy.setFilterText(QString{});
    CHECK(visibleTitles(proxy) == albumOrder());
}

TEST_CASE("sorting by a column with real values still works", "[app][playlist]") {
    Playlist           playlist = makeAlbum();
    PlaylistModel      model{playlist};
    PlaylistProxyModel proxy;
    proxy.setSourceModel(&model);

    proxy.sort(PlaylistModel::ColumnTrack, Qt::DescendingOrder);

    QStringList reversed = albumOrder();
    std::reverse(reversed.begin(), reversed.end());
    CHECK(visibleTitles(proxy) == reversed);
}

TEST_CASE("track numbers sort numerically, not as text", "[app][playlist]") {
    Playlist           playlist = makeAlbum();
    PlaylistModel      model{playlist};
    PlaylistProxyModel proxy;
    proxy.setSourceModel(&model);

    proxy.sort(PlaylistModel::ColumnTrack, Qt::AscendingOrder);

    // Lexicographically "10" precedes "2". Track 10 must still come last.
    CHECK(visibleTitles(proxy).last() == QStringLiteral("Track 10"));
}

// --- removal ------------------------------------------------------------
//
// Playlist notifies its observers *after* it has changed; Qt's model protocol
// wants the opposite, with the rows still present inside beginRemoveRows() and
// rowCount() still reporting the old total. PlaylistModel bridges the two, and
// these are the cases where getting it wrong is not subtle -- Qt asserts.
//
// The crash this was found by: selecting everything and pressing Delete, which
// aborted with `ASSERT: "last < rowCount(parent)"`. The general form is *any*
// removal that includes the final row, so removing the last track alone did it too;
// nothing exercised that, because every existing test removed from the middle.

TEST_CASE("removing the last row does not break the model's row count",
          "[app][playlist]") {
    Playlist      playlist = makeAlbum();
    PlaylistModel model(playlist, nullptr);
    REQUIRE(model.rowCount() == 10);

    // The minimal reproduction. In a debug Qt this aborts rather than fails when
    // the model reads its row count live from the playlist.
    playlist.removeAt(9, 1);
    REQUIRE(model.rowCount() == 9);
    REQUIRE(visibleTitles(model).last() == QStringLiteral("Track 9"));
}

TEST_CASE("removing every row leaves an empty model", "[app][playlist]") {
    // What the bug report was: select all, Delete. Playlist::remove works
    // descending, so the very first erase is the final row -- which is why deleting
    // the whole playlist hit this and deleting most of it did not.
    Playlist      playlist = makeAlbum();
    PlaylistModel model(playlist, nullptr);

    std::vector<TrackId> everything;
    for (const PlaylistEntry& entry : playlist.entries()) {
        everything.push_back(entry.id);
    }
    playlist.remove(everything);

    REQUIRE(model.rowCount() == 0);
    REQUIRE(visibleTitles(model).isEmpty());
    // And the model must stay usable rather than merely surviving the removal.
    REQUIRE_FALSE(
        model.data(model.index(0, PlaylistModel::ColumnTitle), Qt::DisplayRole)
            .isValid());
    REQUIRE(model.trackIdAt(0) == kInvalidTrackId);
}

TEST_CASE("the model's row count tracks the playlist through every edit",
          "[app][playlist]") {
    // The cached count is the price of bridging two protocols that disagree about
    // when to speak, and the risk it carries is drift: one change kind that forgets
    // to advance it and the model reports a stale size for the rest of the session.
    // So this walks one of each.
    Playlist      playlist;
    PlaylistModel model(playlist, nullptr);
    REQUIRE(model.rowCount() == 0);

    playlist = makeAlbum();
    PlaylistModel fresh(playlist, nullptr);
    REQUIRE(fresh.rowCount() == 10);

    // Insert.
    std::vector<PlaylistEntry> extra(1);
    extra[0].url      = *Url::parse("file:///music/extra.flac");
    extra[0].rawTitle = "Extra";
    playlist.insert(playlist.size(), std::move(extra));
    REQUIRE(fresh.rowCount() == 11);

    // Remove from the middle, then from the end.
    playlist.removeAt(0, 1);
    REQUIRE(fresh.rowCount() == 10);
    playlist.removeAt(playlist.size() - 1, 1);
    REQUIRE(fresh.rowCount() == 9);

    // A reordering, which the model answers with a reset.
    playlist.move(0, 1, 5);
    REQUIRE(fresh.rowCount() == 9);

    // And empty again.
    std::vector<TrackId> rest;
    for (const PlaylistEntry& entry : playlist.entries()) {
        rest.push_back(entry.id);
    }
    playlist.remove(rest);
    REQUIRE(fresh.rowCount() == 0);
}
