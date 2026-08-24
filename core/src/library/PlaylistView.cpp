#include "xpcog/core/library/PlaylistView.hpp"

#include "xpcog/core/NaturalOrder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

namespace xpcog {
namespace {

[[nodiscard]] std::string formatDuration(double seconds) {
    if (seconds <= 0.0) {
        return "--:--";
    }
    const auto total   = static_cast<int>(seconds + 0.5);
    const int  minutes = total / 60;
    const int  rest    = total % 60;

    const auto pad = [](int value) {
        std::string text = std::to_string(value);
        return text.size() < 2 ? "0" + text : text;
    };

    if (minutes >= 60) {
        return std::to_string(minutes / 60) + ":" + pad(minutes % 60) + ":" + pad(rest);
    }
    return std::to_string(minutes) + ":" + pad(rest);
}

[[nodiscard]] char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Case-insensitive substring search, ASCII folding.
///
/// Not std::search with a locale-aware predicate: the filter runs over every
/// entry on every keystroke, and this is the same folding naturalLess uses, so
/// the two agree about what "the same text" means.
[[nodiscard]] bool containsFold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t i = 0;
        while (i < needle.size() && lower(haystack[start + i]) == lower(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

}  // namespace

PlaylistView::PlaylistView(Playlist& playlist) : playlist_(playlist) {
    subscription_ = playlist_.observe(
        [this](const Playlist::Change& change) { onPlaylistChanged(change); });
    rebuild();
}

// --- the mapping ---------------------------------------------------------

void PlaylistView::rebuild() {
    visible_.clear();
    visible_.reserve(playlist_.size());

    for (std::size_t index = 0; index < playlist_.size(); ++index) {
        if (matches(playlist_.at(index))) {
            visible_.push_back(index);
        }
    }

    if (sortColumn_ != kNoSort) {
        // Stable, and over the whole mapping at once. Both halves of that matter:
        // stability is what makes playlist order the tie-break without an
        // explicit comparison, and rebuilding whole is what stops an equal run
        // from being reshuffled a row at a time. See the header.
        std::stable_sort(visible_.begin(), visible_.end(),
                         [this](std::size_t a, std::size_t b) { return less(a, b); });
    }
}

bool PlaylistView::matches(const PlaylistEntry& entry) const {
    if (filter_.empty()) {
        return true;
    }
    return containsFold(entry.title(), filter_) || containsFold(entry.artist, filter_) ||
           containsFold(entry.album, filter_);
}

bool PlaylistView::less(std::size_t leftIndex, std::size_t rightIndex) const {
    const PlaylistEntry& left  = playlist_.at(leftIndex);
    const PlaylistEntry& right = playlist_.at(rightIndex);

    // Numeric columns compare as numbers. "4:07" sorts as a string; 247 does not.
    const auto numeric = [this](double a, double b) {
        if (a == b) {
            return false;  // equal: stable_sort keeps playlist order
        }
        return ascending_ ? a < b : b < a;
    };

    switch (sortColumn_) {
        case Column::Status:
            return numeric(static_cast<double>(left.queuePosition),
                           static_cast<double>(right.queuePosition));
        case Column::Track:
            return numeric(static_cast<double>(left.track),
                           static_cast<double>(right.track));
        case Column::Length:
            return numeric(left.duration(), right.duration());

        case Column::Title:
        case Column::Artist:
        case Column::Album: {
            // title() rather than rawTitle: it falls back to the filename when
            // there is no tag, and that fallback is what the column shows. Sorting
            // on the raw field instead makes an untagged album a run of equal
            // empty strings that no ordering can separate.
            const auto textOf = [this](const PlaylistEntry& entry) -> const std::string& {
                switch (sortColumn_) {
                    case Column::Artist: return entry.artist;
                    case Column::Album:  return entry.album;
                    default:             return entry.rawTitle;
                }
            };

            const std::string leftText =
                sortColumn_ == Column::Title ? left.title() : textOf(left);
            const std::string rightText =
                sortColumn_ == Column::Title ? right.title() : textOf(right);

            if (naturalLess(leftText, rightText)) {
                return ascending_;
            }
            if (naturalLess(rightText, leftText)) {
                return !ascending_;
            }
            // Equal: stable_sort keeps playlist order, which is the tie-break.
            return false;
        }

        case Column::Count:
            break;
    }
    return false;
}

void PlaylistView::onPlaylistChanged(const Playlist::Change& change) {
    using Kind = Playlist::Change::Kind;

    switch (change.kind) {
        case Kind::Updated: {
            // In place: the mapping is unchanged unless the filter's answer for
            // one of these rows changed. Checking is cheap and a rebuild here
            // would reset the view once per file during a scan.
            bool mappingStillValid = true;
            for (std::size_t i = 0; i < change.count; ++i) {
                const std::size_t index = change.index + i;
                if (index >= playlist_.size()) {
                    break;
                }
                const bool shown = std::find(visible_.begin(), visible_.end(), index) !=
                                   visible_.end();
                if (shown != matches(playlist_.at(index))) {
                    mappingStillValid = false;
                    break;
                }
            }

            if (!mappingStillValid) {
                rebuild();
                rebuilt.publish();
                return;
            }

            // A sorted view can have the changed rows anywhere, so they are
            // reported by looking each one up rather than by arithmetic.
            for (std::size_t i = 0; i < change.count; ++i) {
                const std::size_t index = change.index + i;
                const auto        found = std::find(visible_.begin(), visible_.end(), index);
                if (found != visible_.end()) {
                    rowChanged.publish(
                        static_cast<std::size_t>(std::distance(visible_.begin(), found)));
                }
            }
            return;
        }

        case Kind::Current:
        case Kind::Queue:
            // The status column only, on every row -- which row was playing is
            // not carried in the change. Cheap: the front end redraws one narrow
            // column of a virtual list.
            for (std::size_t row = 0; row < visible_.size(); ++row) {
                rowChanged.publish(row);
            }
            return;

        case Kind::Inserted:
        case Kind::Removed:
        case Kind::Moved:
        case Kind::Reset:
        case Kind::Order:
            rebuild();
            rebuilt.publish();
            return;
    }
}

// --- reading -------------------------------------------------------------

const PlaylistEntry* PlaylistView::entryAt(std::size_t row) const {
    if (row >= visible_.size()) {
        return nullptr;
    }
    const std::size_t index = visible_[row];
    if (index >= playlist_.size()) {
        return nullptr;
    }
    return &playlist_.at(index);
}

TrackId PlaylistView::trackAt(std::size_t row) const {
    const PlaylistEntry* entry = entryAt(row);
    return entry != nullptr ? entry->id : kInvalidTrackId;
}

std::optional<std::size_t> PlaylistView::rowForTrack(TrackId id) const {
    const auto index = playlist_.indexOf(id);
    if (!index) {
        return std::nullopt;
    }
    const auto found = std::find(visible_.begin(), visible_.end(), *index);
    if (found == visible_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(visible_.begin(), found));
}

std::vector<TrackId> PlaylistView::visibleTracks() const {
    std::vector<TrackId> tracks;
    tracks.reserve(visible_.size());
    for (const std::size_t index : visible_) {
        tracks.push_back(playlist_.at(index).id);
    }
    return tracks;
}

std::vector<PlaylistEntry> PlaylistView::visibleEntries() const {
    std::vector<PlaylistEntry> entries;
    entries.reserve(visible_.size());
    for (const std::size_t index : visible_) {
        entries.push_back(playlist_.at(index));
    }
    return entries;
}

std::string PlaylistView::text(std::size_t row, Column column) const {
    const PlaylistEntry* entry = entryAt(row);
    if (entry == nullptr) {
        return {};
    }

    switch (column) {
        case Column::Status:
            if (entry->id == current_) {
                return "\xE2\x96\xB6";  // U+25B6 BLACK RIGHT-POINTING TRIANGLE
            }
            if (entry->error) {
                return "\xE2\x9A\xA0";  // U+26A0 WARNING SIGN
            }
            if (entry->queued()) {
                return std::to_string(entry->queuePosition + 1);
            }
            return {};
        case Column::Track:
            return entry->track > 0 ? std::to_string(entry->track) : std::string{};
        case Column::Title:  return entry->title();
        case Column::Artist: return entry->artist;
        case Column::Album:  return entry->album;
        case Column::Length: return formatDuration(entry->duration());
        case Column::Count:  break;
    }
    return {};
}

std::string_view PlaylistView::heading(Column column) {
    switch (column) {
        case Column::Status: return "";
        case Column::Track:  return "#";
        case Column::Title:  return "Title";
        case Column::Artist: return "Artist";
        case Column::Album:  return "Album";
        case Column::Length: return "Length";
        case Column::Count:  break;
    }
    return "";
}

// --- arranging -----------------------------------------------------------

void PlaylistView::setFilter(std::string text) {
    if (filter_ == text) {
        return;
    }
    filter_ = std::move(text);
    rebuild();
    rebuilt.publish();
}

void PlaylistView::setSort(Column column, bool ascending) {
    if (sortColumn_ == column && ascending_ == ascending) {
        return;
    }
    sortColumn_ = column;
    ascending_  = ascending;
    rebuild();
    rebuilt.publish();
}

void PlaylistView::setCurrentTrack(TrackId id) {
    if (current_ == id) {
        return;
    }
    const TrackId previous = current_;
    current_               = id;

    // Only the two rows that changed, not the whole column: this fires on every
    // track change, and a full redraw of a long list for two glyphs is waste.
    for (const TrackId affected : {previous, id}) {
        if (affected == kInvalidTrackId) {
            continue;
        }
        if (const auto row = rowForTrack(affected)) {
            rowChanged.publish(*row);
        }
    }
}

}  // namespace xpcog
