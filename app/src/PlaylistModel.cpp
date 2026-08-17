#include "PlaylistModel.hpp"

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
    : QAbstractTableModel(parent), playlist_(playlist) {
    subscription_ = playlist_.observe(
        [this](const Playlist::Change& change) { onPlaylistChanged(change); });
}

void PlaylistModel::onPlaylistChanged(const Playlist::Change& change) {
    using Kind = Playlist::Change::Kind;

    const auto first = static_cast<int>(change.index);
    const auto last  = static_cast<int>(change.index + change.count) - 1;

    switch (change.kind) {
        case Kind::Inserted:
            beginInsertRows({}, first, last);
            endInsertRows();
            break;

        case Kind::Removed:
            beginRemoveRows({}, first, last);
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
    return parent.isValid() ? 0 : static_cast<int>(playlist_.size());
}

int PlaylistModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
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

        // Ids rather than rows: each move shifts every row after it, and
        // recomputing indices between moves is how reordering a multi-row
        // selection quietly scrambles it.
        QList<TrackId> ids;
        ids.reserve(rows.size());
        for (const int source : rows) {
            if (source >= 0 && source < rowCount()) {
                ids.append(trackIdAt(source));
            }
        }

        const TrackId anchor =
            (destination < rowCount()) ? trackIdAt(destination) : kInvalidTrackId;

        // Playlist::move takes the final index of the moved row. Dropping a
        // selection *before* the anchor therefore needs the target adjusted
        // when the row is coming from above it -- removing it first shifts the
        // anchor one place left. Getting this wrong reverses the selection,
        // which looks like the drag picked the rows in the wrong order.
        std::size_t target = (anchor == kInvalidTrackId)
                                 ? playlist_.size()
                                 : playlist_.indexOf(anchor).value_or(playlist_.size());

        for (const TrackId id : ids) {
            const auto from = playlist_.indexOf(id);
            if (!from) {
                continue;
            }
            std::size_t to = target;
            if (*from < to && to > 0) {
                --to;
            }
            playlist_.move(*from, 1, to);
            target = playlist_.indexOf(id).value_or(to) + 1;
        }
        return true;
    }

    if (data->hasUrls()) {
        emit filesDropped(data->urls(), destination);
        return true;
    }
    return false;
}

TrackId PlaylistModel::trackIdAt(int row) const {
    if (row < 0 || row >= rowCount()) {
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
        return a.toDouble() < b.toDouble();
    }

    return collator_.compare(sourceModel()->data(left, Qt::DisplayRole).toString(),
                             sourceModel()->data(right, Qt::DisplayRole).toString()) < 0;
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
