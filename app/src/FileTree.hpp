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
class QToolButton;
class QTreeView;

namespace xpcog::app {

class FileTree : public QWidget {
    Q_OBJECT

public:
    FileTree(const PluginRegistry& registry, QWidget* parent = nullptr);

    /// The folder shown at the root. Persisted by the window across launches.
    void          setRootPath(const QString& path);
    [[nodiscard]] QString rootPath() const;

public slots:
    /// Asks for a new root folder. Cog puts this behind a bare "Choose" button
    /// above its outline view (FileTree.xib, -chooseRootFolder:); here it is the
    /// header button, the context menu, and the View menu, since a button
    /// labelled with the folder you are in answers "where am I" at the same time
    /// as offering to change it.
    void chooseRootPath();

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
    /// Shows the current root and opens the chooser. Its text is the folder's
    /// name, which is otherwise invisible: setRootIndex() shows a folder's
    /// *contents*, so without this the tree never says what it is showing.
    QToolButton*          root_  = nullptr;
};

}  // namespace xpcog::app
