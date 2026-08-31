// What the playlist looks like on screen: which rows are shown, in what order.
//
// This is the half of the old Qt PlaylistModel that was not Qt. It held a
// QAbstractTableModel over the playlist and a QSortFilterProxyModel over that,
// and between them they decided the row order, the filtering and the cell text.
// None of that is toolkit work, and moving it here means it can be tested
// without a display -- which matters, because two of the sharpest bugs this
// project has had were in exactly this code.
//
// **Sort order is display-only.** That is XPCog's one deliberate behaviour
// difference from Cog, which shuffles and steps through the *sorted* order.
// Playback reads Playlist; this reads Playlist and rearranges a vector of
// indices. The two cannot interfere, and now they cannot even reach each other:
// nothing in this class is non-const on the playlist.
//
// --- Two behaviours worth keeping, both of them bugs once -----------------
//
// The tie-break is playlist order, and the sort is stable. Sorting an album by
// artist -- ten rows, one artist, every comparison a tie -- otherwise leaves
// their relative order to whatever the sort happens to do. It looks fine until
// the filter changes, because the old proxy re-inserted surviving rows one at a
// time and each landed at an arbitrary point in the equal run. The symptom was
// that typing in the filter box and clearing it again shuffled the playlist.
// Here the mapping is rebuilt whole with std::stable_sort, which makes that
// impossible rather than defended against.
//
// Numeric columns compare as numbers, not as their formatted text: "4:07" sorts
// as a string, 247 does not. Text columns compare with naturalLess, so "Track 9"
// precedes "Track 10" -- core already had that comparator for the scanner.
// QCollator did it before, and did it locale-sensitively, which is the one thing
// lost here; naturalLess is ASCII case-folding, so `Ágætis` sorts after `Zoo`.
// Recorded as a regression in docs/WXPORT.md rather than papered over.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {

/// Does this entry match `needle`? Substring across title, artist and album,
/// case-insensitive for ASCII -- the same question the filter box asks.
///
/// Exposed as a free function because there is a second searcher now: the REST
/// remote control's `?q=`. It must not reach through setFilter(), because a
/// remote *read* that changed what the user is looking at would be a surprise of
/// exactly the kind this project documents against -- so it needs the matcher
/// without the view's state, and both sides then agree on what "matches" means
/// rather than reimplementing it slightly differently.
///
/// An empty needle matches everything, which is what "no filter" means.
[[nodiscard]] bool playlistEntryMatches(const PlaylistEntry& entry,
                                        std::string_view     needle);

class PlaylistView {
public:
    enum class Column {
        Status = 0,  ///< playing / queued marker
        Track,
        Title,
        Artist,
        Album,
        Length,
        Count,
    };

    /// `Column::Count` as a sort column means "no sort": rows appear in playlist
    /// order. That is the initial state, and what a third click on a header
    /// should return to.
    static constexpr Column kNoSort = Column::Count;

    explicit PlaylistView(Playlist& playlist);

    // --- what is shown -----------------------------------------------------

    [[nodiscard]] std::size_t rowCount() const noexcept { return visible_.size(); }

    /// The entry at a visible row, or null when the row is out of range.
    [[nodiscard]] const PlaylistEntry* entryAt(std::size_t row) const;

    [[nodiscard]] TrackId trackAt(std::size_t row) const;

    /// Where a track appears, or nullopt when it is filtered out or absent.
    [[nodiscard]] std::optional<std::size_t> rowForTrack(TrackId id) const;

    /// Every visible row's track, top to bottom. What a "select all" acts on.
    [[nodiscard]] std::vector<TrackId> visibleTracks() const;

    /// Every visible row's entry, top to bottom. What saving a playlist file
    /// writes: the sort the listener applied and the rows the filter leaves,
    /// which is the order they are looking at.
    ///
    /// Copies, where the rest of this class hands out references. The rows it
    /// names are scattered through the playlist, so there is no contiguous range
    /// to point at, and writePlaylist wants a vector of entries. Saving happens
    /// once per keystroke on Ctrl+S rather than once per redraw, so the copy is
    /// the cheap side of the trade.
    [[nodiscard]] std::vector<PlaylistEntry> visibleEntries() const;

    /// The cell's text, already formatted -- a duration as `4:07`, a missing
    /// value as empty. The one piece of presentation core owns, because the
    /// alternative is every front end formatting a duration slightly differently.
    [[nodiscard]] std::string text(std::size_t row, Column column) const;

    /// The column headings, in order. Untranslated: core has no catalogue, and a
    /// front end that wants them localised should map them.
    [[nodiscard]] static std::string_view heading(Column column);

    // --- how it is arranged -------------------------------------------------

    /// Substring match across title, artist and album, case-insensitive for
    /// ASCII. Replaces Cog's SpotlightPanel and its 965 lines of NSMetadataQuery.
    void setFilter(std::string text);
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

    void setSort(Column column, bool ascending);
    [[nodiscard]] Column sortColumn() const noexcept { return sortColumn_; }
    [[nodiscard]] bool   sortAscending() const noexcept { return ascending_; }

    /// Highlights the playing row. Not stored on the entry: it is view state,
    /// and Cog storing `current` on the managed object is why its playlist has
    /// to be re-saved every time playback advances.
    void setCurrentTrack(TrackId id);
    [[nodiscard]] TrackId currentTrack() const noexcept { return current_; }

    // --- notification -------------------------------------------------------

    /// The mapping changed: row count, order, or which rows are shown. Whatever
    /// is displaying this must reload from scratch.
    ///
    /// Coarser than a per-row notification (wxDataViewVirtualListModel's
    /// RowInserted), and deliberately. With a sort and a
    /// filter in the way, an insertion at playlist index 3 can land anywhere or
    /// nowhere, so computing a precise row delta means diffing the old mapping
    /// against the new one -- real work, to save a redraw of a list that is
    /// already virtual. What it costs is the selection, which is why the
    /// in-place case below is kept separate.
    Signal<> rebuilt;

    /// One visible row's contents changed, with the mapping intact. A tag read
    /// finishing during a scan, or the playing row moving.
    ///
    /// This is the case that must not be a rebuild: a scan publishes one of these
    /// per file, and resetting the whole view that often would make the list
    /// unusable while it ran.
    Signal<std::size_t> rowChanged;

private:
    void rebuild();
    void onPlaylistChanged(const Playlist::Change& change);

    /// True when the entry passes the current filter.
    [[nodiscard]] bool matches(const PlaylistEntry& entry) const;

    /// Orders two playlist indices by the current sort column.
    [[nodiscard]] bool less(std::size_t leftIndex, std::size_t rightIndex) const;

    Playlist&    playlist_;
    Subscription subscription_;

    /// Playlist indices, in display order. The whole of what this class is.
    std::vector<std::size_t> visible_;

    std::string filter_;
    Column      sortColumn_ = kNoSort;
    bool        ascending_  = true;
    TrackId     current_    = kInvalidTrackId;
};

}  // namespace xpcog
