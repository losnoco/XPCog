#include "PlaylistModel.hpp"

#include "PlaylistCommands.hpp"

#include <QDataStream>
#include <QtGlobal>
#include <QIODevice>
#include <QMimeData>
#include <QUrl>

#include <algorithm>

namespace xpcog::app {
namespace {

constexpr auto kInternalMime = "application/x-xpcog-playlist-rows";

[[nodiscard]] QString formatDuration(double seconds) {
    if (seconds <= 0.0) {
        return QStringLiteral("--:--");
    }
    const auto total   = static_cast<int>(seconds + 0.5);
    const int  minutes = total / 60;
    const int  rest    = total % 60;
    if (minutes >= 60) {
        return QStringLiteral("%1:%2:%3")
            .arg(minutes / 60)
            .arg(minutes % 60, 2, 10, QLatin1Char('0'))
            .arg(rest, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString toQt(const std::string& text) {
    return QString::fromStdString(text);
}

}  // namespace

// --- PlaylistModel ------------------------------------------------------

PlaylistModel::PlaylistModel(Playlist& playlist, QObject* parent)
    : QAbstractTableModel(parent),
      playlist_(playlist),
      rows_(static_cast<int>(playlist.size())) {
    subscription_ = playlist_.observe(
        [this](const Playlist::Change& change) { onPlaylistChanged(change); });
}

void PlaylistModel::onPlaylistChanged(const Playlist::Change& change) {
    using Kind = Playlist::Change::Kind;

    const auto first = static_cast<int>(change.index);
    const auto last  = static_cast<int>(change.index + change.count) - 1;

    // The playlist has already changed by the time this runs, so `rows_` still
    // holds the count Qt must see inside the begin call, and is advanced to the new
    // one before the matching end. See the declaration for what goes wrong if this
    // reads the playlist live instead.
    const auto updated = static_cast<int>(playlist_.size());

    switch (change.kind) {
        case Kind::Inserted:
            beginInsertRows({}, first, last);
            rows_ = updated;
            endInsertRows();
            break;

        case Kind::Removed:
            beginRemoveRows({}, first, last);
            rows_ = updated;
            endRemoveRows();
            break;

        case Kind::Updated:
            emit dataChanged(index(first, 0), index(last, ColumnCount - 1));
            break;

        case Kind::Moved:
        case Kind::Reset:
        case Kind::Order:
            // A move is expressible as beginMoveRows, but only for a
            // contiguous run that does not straddle the destination, and
            // Playlist::move allows both. A reset is correct in every case and
            // a playlist edit is not a hot path.
            beginResetModel();
            rows_ = updated;
            endResetModel();
            break;

        case Kind::Current:
        case Kind::Queue:
            // Status column only.
            if (rowCount() > 0) {
                emit dataChanged(index(0, ColumnStatus),
                                 index(rowCount() - 1, ColumnStatus));
            }
            break;
    }
}

int PlaylistModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_;
}

int PlaylistModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    // Against the playlist, not rowCount(). The two disagree for the moment
    // between beginRemoveRows() and endRemoveRows(), where rowCount() deliberately
    // still reports the pre-removal count -- and this indexes into the playlist,
    // which has already shrunk.
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= playlist_.size()) {
        return {};
    }
    const PlaylistEntry& entry = playlist_.at(static_cast<std::size_t>(index.row()));

    switch (role) {
        case TrackIdRole:
            return QVariant::fromValue(static_cast<qulonglong>(entry.id));

        case IsCurrentRole:
            return entry.id == current_;

        case SortRole:
            // The underlying value, so numbers sort as numbers.
            switch (index.column()) {
                case ColumnTrack:  return entry.track;
                case ColumnLength: return entry.duration();
                case ColumnStatus: return entry.queuePosition;
                default: break;
            }
            break;

        case Qt::TextAlignmentRole:
            if (index.column() == ColumnTrack || index.column() == ColumnLength) {
                return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
            }
            break;

        case Qt::ToolTipRole:
            if (entry.error) {
                return toQt(entry.errorMessage);
            }
            return toQt(entry.url.toString());

        case Qt::DisplayRole:
            switch (index.column()) {
                case ColumnStatus:
                    if (entry.id == current_) {
                        return QStringLiteral("▶");  // ▶
                    }
                    if (entry.error) {
                        return QStringLiteral("⚠");  // ⚠
                    }
                    if (entry.queued()) {
                        return QString::number(entry.queuePosition + 1);
                    }
                    return {};
                case ColumnTrack:
                    return entry.track > 0 ? QString::number(entry.track) : QString{};
                case ColumnTitle:  return toQt(entry.title());
                case ColumnArtist: return toQt(entry.artist);
                case ColumnAlbum:  return toQt(entry.album);
                case ColumnLength: return formatDuration(entry.duration());
                default: break;
            }
            break;

        default:
            break;
    }
    return {};
}

QVariant PlaylistModel::headerData(int section, Qt::Orientation orientation,
                                   int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
        case ColumnStatus: return QString{};
        case ColumnTrack:  return tr("#");
        case ColumnTitle:  return tr("Title");
        case ColumnArtist: return tr("Artist");
        case ColumnAlbum:  return tr("Album");
        case ColumnLength: return tr("Length");
        default: return {};
    }
}

Qt::ItemFlags PlaylistModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags base = QAbstractTableModel::flags(index);
    if (index.isValid()) {
        return base | Qt::ItemIsDragEnabled;
    }
    return base | Qt::ItemIsDropEnabled;
}

// --- drag and drop ------------------------------------------------------

Qt::DropActions PlaylistModel::supportedDropActions() const {
    return Qt::MoveAction | Qt::CopyAction;
}

QStringList PlaylistModel::mimeTypes() const {
    return {QString::fromLatin1(kInternalMime), QStringLiteral("text/uri-list")};
}

QMimeData* PlaylistModel::mimeData(const QModelIndexList& indexes) const {
    auto* data = new QMimeData;

    QByteArray   encoded;
    QDataStream  stream(&encoded, QIODevice::WriteOnly);
    QList<QUrl>  urls;

    // Rows, not ids, because the drop needs positions -- but deduplicated,
    // since a selection yields one index per column.
    QList<int> rows;
    for (const QModelIndex& index : indexes) {
        if (index.isValid() && !rows.contains(index.row())) {
            rows.append(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());

    for (const int row : rows) {
        stream << row;
        if (const auto path = playlist_.at(static_cast<std::size_t>(row)).url.localPath()) {
            urls.append(QUrl::fromLocalFile(QString::fromStdString(path->string())));
        }
    }

    data->setData(QString::fromLatin1(kInternalMime), encoded);
    if (!urls.isEmpty()) {
        // So a drag out of the playlist into another application carries files.
        data->setUrls(urls);
    }
    return data;
}

bool PlaylistModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                 int /*column*/, const QModelIndex& parent) {
    if (action == Qt::IgnoreAction || data == nullptr) {
        return false;
    }

    int destination = row;
    if (destination < 0) {
        destination = parent.isValid() ? parent.row() : rowCount();
    }

    if (data->hasFormat(QString::fromLatin1(kInternalMime))) {
        QByteArray  encoded = data->data(QString::fromLatin1(kInternalMime));
        QDataStream stream(&encoded, QIODevice::ReadOnly);

        QList<int> rows;
        while (!stream.atEnd()) {
            int value = 0;
            stream >> value;
            rows.append(value);
        }
        if (rows.isEmpty()) {
            return false;
        }

        // Ids rather than rows: the drop is turned into a target order, and
        // rows stop meaning anything the moment the first one moves.
        std::vector<TrackId> ids;
        ids.reserve(static_cast<std::size_t>(rows.size()));
        for (const int source : rows) {
            if (source >= 0 && source < rowCount()) {
                ids.push_back(trackIdAt(source));
            }
        }
        if (ids.empty()) {
            return false;
        }

        const TrackId anchor =
            (destination < rowCount()) ? trackIdAt(destination) : kInvalidTrackId;

        emit reorderRequested(orderAfterMove(playlist_, ids, anchor));
        return true;
    }

    if (data->hasUrls()) {
        emit filesDropped(data->urls(), destination);
        return true;
    }
    return false;
}

TrackId PlaylistModel::trackIdAt(int row) const {
    // The playlist's bounds, for the same reason as data().
    if (row < 0 || static_cast<std::size_t>(row) >= playlist_.size()) {
        return kInvalidTrackId;
    }
    return playlist_.at(static_cast<std::size_t>(row)).id;
}

int PlaylistModel::rowForTrack(TrackId id) const {
    const auto index = playlist_.indexOf(id);
    return index ? static_cast<int>(*index) : -1;
}

void PlaylistModel::setCurrentTrack(TrackId id) {
    if (current_ == id) {
        return;
    }
    current_ = id;
    if (rowCount() > 0) {
        emit dataChanged(index(0, ColumnStatus), index(rowCount() - 1, ColumnStatus),
                         {Qt::DisplayRole, IsCurrentRole});
    }
}

// --- PlaylistProxyModel -------------------------------------------------

PlaylistProxyModel::PlaylistProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {
    collator_.setNumericMode(true);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
    setSortRole(PlaylistModel::SortRole);
    setDynamicSortFilter(true);
}

void PlaylistProxyModel::setFilterText(const QString& text) {
    if (filter_ == text) {
        return;
    }
    // begin/endFilterChange is the Qt 6.9 replacement for invalidateFilter(),
    // which 6.13 deprecates. Guarded rather than simply requiring 6.9: the
    // project builds against 6.5 and up, and CI pins 6.8.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    filter_ = text;
    endFilterChange();
#else
    filter_ = text;
    invalidateFilter();
#endif
}

bool PlaylistProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const QVariant a = sourceModel()->data(left, PlaylistModel::SortRole);
    const QVariant b = sourceModel()->data(right, PlaylistModel::SortRole);

    // Numeric columns expose a SortRole value; text columns do not and fall
    // through to the collator, which is what makes "Track 9" precede "Track 10".
    if (a.isValid() && b.isValid()) {
        const double x = a.toDouble();
        const double y = b.toDouble();
        if (x != y) {
            return x < y;
        }
    } else {
        const int order =
            collator_.compare(sourceModel()->data(left, Qt::DisplayRole).toString(),
                              sourceModel()->data(right, Qt::DisplayRole).toString());
        if (order != 0) {
            return order < 0;
        }
    }

    // Ties fall back to playlist order, which makes the comparison a total
    // order rather than a partial one.
    //
    // Without this, sorting an album by artist -- ten rows, one artist, every
    // comparison a tie -- leaves the relative order of those rows up to
    // whatever the proxy's binary search happens to do. It looks stable until
    // the filter changes, because a filter change re-inserts the surviving rows
    // one at a time and each lands at an arbitrary point in the equal run. The
    // symptom is that typing in the filter box and clearing it again shuffles
    // the playlist.
    return left.row() < right.row();
}

bool PlaylistProxyModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
    if (filter_.isEmpty()) {
        return true;
    }
    for (const int column : {PlaylistModel::ColumnTitle, PlaylistModel::ColumnArtist,
                             PlaylistModel::ColumnAlbum}) {
        const QString text =
            sourceModel()->data(sourceModel()->index(row, column, parent)).toString();
        if (text.contains(filter_, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace xpcog::app
