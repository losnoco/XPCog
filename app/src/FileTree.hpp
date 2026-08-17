// The folder browser. Replaces Cog's FileTree/ (1,431 lines, built on FSEvents
// and a hand-rolled node cache) with QFileSystemModel, which does the same job
// including the watching.
//
// Filtered to what the registry can actually decode, so browsing a music folder
// shows music rather than cover art and log files. The filter comes from the
// registry rather than a fixed list, so a new codec appears here with no edit.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"

#include <QUrl>
#include <QWidget>

class QFileSystemModel;
class QTreeView;

namespace xpcog::app {

class FileTree : public QWidget {
    Q_OBJECT

public:
    FileTree(const PluginRegistry& registry, QWidget* parent = nullptr);

    /// The folder shown at the root. Persisted by the window across launches.
    void          setRootPath(const QString& path);
    [[nodiscard]] QString rootPath() const;

signals:
    /// Double-clicked, or Enter pressed. Folders come through too -- the
    /// scanner expands them, so the tree does not need to know the difference.
    void activated(const QList<QUrl>& urls);

    /// The context menu's "Add to Playlist".
    void addRequested(const QList<QUrl>& urls);

private:
    [[nodiscard]] QList<QUrl> selectedUrls() const;

    const PluginRegistry& registry_;
    QFileSystemModel*     model_ = nullptr;
    QTreeView*            view_  = nullptr;
};

}  // namespace xpcog::app
