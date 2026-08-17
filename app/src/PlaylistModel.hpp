// The playlist as a Qt table. Replaces roughly sixty of Cog's XIB bindings in
// one class -- every column that was a separate <binding> to a PlaylistEntry
// keyPath is now a case in data().
//
// The model holds no state of its own. xpcog::Playlist owns the order and the
// contents; this translates Playlist::Change into begin/endInsertRows and
// dataChanged. That split is what keeps sort order display-only: the proxy above
// can arrange rows however it likes without playback noticing, which is the one
// deliberate behaviour change from Cog.

#pragma once

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/library/Playlist.hpp"

#include <QAbstractTableModel>
#include <QList>
#include <QUrl>
#include <QCollator>
#include <QSortFilterProxyModel>

namespace xpcog::app {

class PlaylistModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        ColumnStatus = 0,  ///< playing / queued marker
        ColumnTrack,
        ColumnTitle,
        ColumnArtist,
        ColumnAlbum,
        ColumnLength,
        ColumnCount,
    };

    /// Beyond Qt::DisplayRole, so the proxy can sort on the underlying value
    /// rather than on formatted text -- "4:07" sorts as a string, 247 does not.
    enum Role {
        SortRole = Qt::UserRole + 1,
        TrackIdRole,
        IsCurrentRole,
    };

    explicit PlaylistModel(Playlist& playlist, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    // --- drag and drop, replacing Cog's DNDArrayController ---------------
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] QStringList     mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

    [[nodiscard]] TrackId trackIdAt(int row) const;
    [[nodiscard]] int     rowForTrack(TrackId id) const;

signals:
    /// Files were dropped from outside the application. The model does not scan
    /// them itself: that needs the registry and a Library, which belong to the
    /// window, not to a table model.
    void filesDropped(const QList<QUrl>& urls, int row);

    /// A drag within the playlist wants the rows in this order. The model does
    /// not apply it either: a drag is one undoable edit, and the undo stack
    /// belongs to the window.
    void reorderRequested(const std::vector<TrackId>& order);

public:
    /// Highlights the playing row. Not stored on the entry: it is view state,
    /// and Cog storing `current` on the managed object is why its playlist has
    /// to be re-saved every time playback advances.
    void setCurrentTrack(TrackId id);

private:
    void onPlaylistChanged(const Playlist::Change& change);

    Playlist&    playlist_;
    Subscription subscription_;
    TrackId      current_ = kInvalidTrackId;

    /// The row count this model reports, which is *not* always what the playlist
    /// currently holds.
    ///
    /// Playlist notifies after the fact: `removeAt` erases, reindexes and only then
    /// calls its observers. Qt's protocol is the other way round -- inside
    /// beginRemoveRows() the rows must still exist, and rowCount() must still
    /// return the count from before. Reading the playlist live therefore fails
    /// `ASSERT: "last < rowCount(parent)"` for any removal that includes the final
    /// row, which is every "select all and delete".
    ///
    /// So this adapter keeps the number Qt expects to see and advances it between
    /// begin and end. Every path that changes the count must maintain it, which is
    /// the cost of bridging two protocols that disagree about when to speak.
    int rows_ = 0;
};

/// Sorting and filtering, display-only.
class PlaylistProxyModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit PlaylistProxyModel(QObject* parent = nullptr);

    /// Substring match across title, artist and album. Replaces Cog's
    /// SpotlightPanel and its 965 lines of NSMetadataQuery.
    void setFilterText(const QString& text);

protected:
    [[nodiscard]] bool lessThan(const QModelIndex& left,
                                const QModelIndex& right) const override;
    [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
    /// Constructed once. Building a QCollator per comparison is a known Qt
    /// performance trap -- it is far more expensive than the comparison itself,
    /// and a sort does O(n log n) of them.
    QCollator collator_;
    QString   filter_;
};

}  // namespace xpcog::app
