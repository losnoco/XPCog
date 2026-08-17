#include "FileTree.hpp"

#include <QAction>
#include <QDir>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QStandardPaths>
#include <QTreeView>
#include <QVBoxLayout>

namespace xpcog::app {

FileTree::FileTree(const PluginRegistry& registry, QWidget* parent)
    : QWidget(parent), registry_(registry) {
    model_ = new QFileSystemModel(this);
    model_->setResolveSymlinks(false);

    // Names, not a hard filter: setNameFilters with setNameFilterDisables(false)
    // hides non-matching *files* while leaving folders navigable, which is what
    // a music browser wants. Cog reimplements this over FSEvents.
    QStringList patterns;
    for (const std::string& extension : registry_.allExtensions()) {
        patterns << QStringLiteral("*.%1").arg(QString::fromStdString(extension));
    }
    patterns << QStringLiteral("*.cue") << QStringLiteral("*.m3u")
             << QStringLiteral("*.m3u8") << QStringLiteral("*.pls")
             << QStringLiteral("*.xspf");
    model_->setNameFilters(patterns);
    model_->setNameFilterDisables(false);

    view_ = new QTreeView(this);
    view_->setModel(model_);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setDragEnabled(true);
    view_->setDragDropMode(QAbstractItemView::DragOnly);
    view_->setHeaderHidden(true);
    // Size, type and date belong in a file manager, not beside a playlist.
    for (int column = 1; column < model_->columnCount(); ++column) {
        view_->hideColumn(column);
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);

    connect(view_, &QTreeView::activated, this,
            [this](const QModelIndex&) { emit activated(selectedUrls()); });

    auto* add = new QAction(tr("Add to Playlist"), this);
    connect(add, &QAction::triggered, this, [this] { emit addRequested(selectedUrls()); });
    view_->addAction(add);
    view_->setContextMenuPolicy(Qt::ActionsContextMenu);

    setRootPath(QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
}

void FileTree::setRootPath(const QString& path) {
    const QString target =
        (path.isEmpty() || !QDir{path}.exists()) ? QDir::homePath() : path;
    // setRootPath tells the model what to watch; setRootIndex tells the view
    // where to start. Both are needed, and doing only the first is a common way
    // to end up browsing the whole filesystem.
    view_->setRootIndex(model_->setRootPath(target));
}

QString FileTree::rootPath() const { return model_->rootPath(); }

QList<QUrl> FileTree::selectedUrls() const {
    QList<QUrl> urls;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows()) {
        urls.append(QUrl::fromLocalFile(model_->filePath(index)));
    }
    return urls;
}

}  // namespace xpcog::app
