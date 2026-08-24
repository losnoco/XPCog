// The playlist model's one piece of behaviour: it refuses to sort.
//
// Here rather than in the core suite because what is being tested is the seam
// with wxDataViewCtrl. The order itself is PlaylistView's and is tested there;
// this is about the control's own sorting, which a sortable column turns on and
// which nothing in core can see.
//
// The bug this pins down was visible and confusing: the track column read 1, 10,
// 100, 1000, 2 on macOS. PlaylistView had sorted the rows numerically, and then
// the control sorted them again through wxDataViewModel::Compare, whose default
// compares the formatted cell text -- so the second sort undid the first. The
// model's own Compare() answers with row order, which makes that second sort a
// no-op instead.

#include "PlaylistDataModel.hpp"

#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

using namespace xpcog;
using namespace xpcog::app;

namespace {

using Column = PlaylistView::Column;

constexpr unsigned int kTrackColumn = static_cast<unsigned int>(Column::Track);

/// Track numbers of one, two and three digits, in an order no accident would
/// sort correctly: text order and number order disagree everywhere.
Playlist makeUnevenTracks() {
    Playlist                   playlist;
    std::vector<PlaylistEntry> entries;
    for (int number : {2, 100, 9, 1000, 1, 10}) {
        PlaylistEntry entry;
        entry.url = *Url::parse("file:///music/" + std::to_string(number) + ".flac");
        entry.rawTitle = "Track " + std::to_string(number);
        entry.track    = number;
        entries.push_back(std::move(entry));
    }
    playlist.insert(0, std::move(entries));
    return playlist;
}

/// What the control ends up showing: the rows as the model orders them under its
/// own Compare(), read back through the same GetValue() the cells are drawn from.
///
/// `ascending` is the flag the header arrow is in, not the view's sort direction.
/// The two are separate, which is the point of passing it.
std::vector<std::string> columnAsSorted(PlaylistDataModel& model, unsigned int column,
                                        bool ascending) {
    std::vector<unsigned int> rows(model.GetCount());
    std::iota(rows.begin(), rows.end(), 0U);

    std::sort(rows.begin(), rows.end(), [&](unsigned int a, unsigned int b) {
        return model.Compare(model.GetItem(a), model.GetItem(b), column, ascending) < 0;
    });

    std::vector<std::string> values;
    for (unsigned int row : rows) {
        wxVariant value;
        model.GetValueByRow(value, row, column);
        values.push_back(std::string{value.GetString().ToUTF8()});
    }
    return values;
}

}  // namespace

TEST_CASE("the control's own sort leaves track numbers in numeric order",
          "[wx][playlist]") {
    Playlist     playlist = makeUnevenTracks();
    PlaylistView view{playlist};
    view.setSort(Column::Track, true);

    auto* model = new PlaylistDataModel{view};

    const std::vector<std::string> numeric = {"1", "2", "9", "10", "100", "1000"};
    CHECK(columnAsSorted(*model, kTrackColumn, true) == numeric);

    // Descending is already in the mapping -- the view was told about it when the
    // header was clicked -- so the arrow's direction must not reverse it a second
    // time. This is the case that read 1000, 100, 10, 1, 9, 2 before.
    CHECK(columnAsSorted(*model, kTrackColumn, false) == numeric);

    model->DecRef();
}

TEST_CASE("the descending order the view produced survives the control",
          "[wx][playlist]") {
    Playlist     playlist = makeUnevenTracks();
    PlaylistView view{playlist};
    view.setSort(Column::Track, false);

    auto* model = new PlaylistDataModel{view};

    const std::vector<std::string> reversed = {"1000", "100", "10", "9", "2", "1"};
    CHECK(columnAsSorted(*model, kTrackColumn, true) == reversed);
    CHECK(columnAsSorted(*model, kTrackColumn, false) == reversed);

    model->DecRef();
}

TEST_CASE("titles keep the view's natural order under every column",
          "[wx][playlist]") {
    Playlist     playlist = makeUnevenTracks();
    PlaylistView view{playlist};
    view.setSort(Column::Title, true);

    auto* model = new PlaylistDataModel{view};

    // naturalLess put "Track 9" before "Track 10"; comparing the text would put
    // it after "Track 1000". Asking about a column the view is not sorted by --
    // the album, every row of which is empty here -- must not reorder anything
    // either, which is why both are checked.
    const std::vector<std::string> natural = {"Track 1",   "Track 2",   "Track 9",
                                              "Track 10",  "Track 100", "Track 1000"};
    CHECK(columnAsSorted(*model, static_cast<unsigned int>(Column::Title), true) ==
          natural);
    CHECK(columnAsSorted(*model, static_cast<unsigned int>(Column::Album), true) ==
          std::vector<std::string>(natural.size(), std::string{}));

    model->DecRef();
}
