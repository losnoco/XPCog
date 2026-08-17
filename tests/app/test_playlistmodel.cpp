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
