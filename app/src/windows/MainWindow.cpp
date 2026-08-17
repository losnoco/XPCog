#include "MainWindow.hpp"

#include "ActionRegistry.hpp"
#include "PlaybackController.hpp"
#include "PlaylistModel.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/core/library/Scanner.hpp"
#include "xpcog/platform/QSettingsStore.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMimeData>
#include <QProgressDialog>
#include <QSlider>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace xpcog::app {
namespace {

[[nodiscard]] QString formatClock(double seconds) {
    const auto total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60)
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

/// The seek slider works in whole seconds. Sub-second resolution on a bar a few
/// hundred pixels wide is below what anyone can aim at.
constexpr int kSeekScale = 1;

}  // namespace

MainWindow::MainWindow(const PluginRegistry& registry, Settings& settings, QWidget* parent)
    : QMainWindow(parent), registry_(registry), settings_(settings) {
    setWindowTitle(QStringLiteral("XPCog"));
    resize(1100, 680);
    setAcceptDrops(true);

    library_ = std::make_unique<Library>();
    if (!library_->open(platform::libraryDatabasePath())) {
        // A library that will not open is not fatal: the player still plays,
        // it just will not remember the playlist. Saying so once beats failing
        // to launch.
        statusBar()->showMessage(
            tr("Library unavailable: %1").arg(QString::fromStdString(library_->lastError())),
            10000);
        library_.reset();
    }

    playback_ = std::make_unique<PlaybackController>(registry_, playlist_, settings_, this);

    buildUi();
    buildMenus();
    wireUp();

    if (library_ && library_->loadPlaylist(playlist_)) {
        statusBar()->showMessage(statusSummary());
    }

    // Modes come from settings, which hold Cog's integers.
    playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
    playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
    playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());
}

MainWindow::~MainWindow() = default;

// --- construction -------------------------------------------------------

void MainWindow::buildUi() {
    model_ = new PlaylistModel(playlist_, this);
    proxy_ = new PlaylistProxyModel(this);
    proxy_->setSourceModel(model_);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setSortingEnabled(true);
    view_->setAlternatingRowColors(true);
    view_->setDragEnabled(true);
    view_->setAcceptDrops(true);
    view_->setDropIndicatorShown(true);
    view_->setDragDropMode(QAbstractItemView::DragDrop);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(false);
    view_->horizontalHeader()->setSectionResizeMode(PlaylistModel::ColumnTitle,
                                                    QHeaderView::Stretch);
    view_->setColumnWidth(PlaylistModel::ColumnStatus, 28);
    view_->setColumnWidth(PlaylistModel::ColumnTrack, 44);
    view_->setColumnWidth(PlaylistModel::ColumnLength, 72);

    setCentralWidget(view_);

    auto* transport = addToolBar(tr("Transport"));
    transport->setMovable(false);

    seekBar_ = new QSlider(Qt::Horizontal, transport);
    seekBar_->setEnabled(false);
    seekBar_->setMinimumWidth(220);

    clock_ = new QLabel(QStringLiteral("0:00 / 0:00"), transport);
    clock_->setMinimumWidth(90);
    clock_->setAlignment(Qt::AlignCenter);

    volume_ = new QSlider(Qt::Horizontal, transport);
    volume_->setRange(0, 100);
    volume_->setValue(static_cast<int>(settings_.Volume() * 100.0));
    volume_->setMaximumWidth(110);
    volume_->setToolTip(tr("Volume"));

    filter_ = new QLineEdit(transport);
    filter_->setPlaceholderText(tr("Filter"));
    filter_->setClearButtonEnabled(true);
    filter_->setMaximumWidth(200);

    nowPlaying_ = new QLabel(this);
    statusBar()->addPermanentWidget(nowPlaying_);

    // The toolbar is assembled after the actions exist, in buildMenus().
    transport->addSeparator();
    transport->addWidget(seekBar_);
    transport->addWidget(clock_);
    transport->addSeparator();
    transport->addWidget(volume_);
    transport->addWidget(filter_);
}

void MainWindow::buildMenus() {
    actions_ = new ActionRegistry(this);
    actions_->populateMenuBar(menuBar());

    // The transport buttons go at the head of the toolbar built above.
    if (auto* transport = findChild<QToolBar*>(); transport != nullptr) {
        QAction* first = transport->actions().isEmpty() ? nullptr
                                                        : transport->actions().front();
        for (const ActionId id : {ActionId::PlaybackPrevious, ActionId::PlaybackPlayPause,
                                  ActionId::PlaybackStop, ActionId::PlaybackNext}) {
            if (QAction* command = actions_->action(id); command != nullptr) {
                transport->insertAction(first, command);
            }
        }
    }

    // The playlist gets the same commands as a context menu -- the same QAction
    // objects, so they cannot drift apart.
    view_->setContextMenuPolicy(Qt::ActionsContextMenu);
    for (const ActionId id : {ActionId::PlaybackPlayPause, ActionId::PlaybackEnqueue,
                              ActionId::EditRemove}) {
        if (QAction* command = actions_->action(id); command != nullptr) {
            view_->addAction(command);
        }
    }
}

void MainWindow::wireUp() {
    const auto on = [this](ActionId id, auto&& slot) {
        if (QAction* command = actions_->action(id); command != nullptr) {
            connect(command, &QAction::triggered, this, slot);
        }
    };

    on(ActionId::FileOpen, [this] { openFiles(); });
    on(ActionId::FileOpenFolder, [this] { openFolder(); });
    on(ActionId::FileQuit, [] { QApplication::quit(); });
    on(ActionId::EditSelectAll, [this] { view_->selectAll(); });
    on(ActionId::EditRemove, [this] { removeSelected(); });
    on(ActionId::PlaybackEnqueue, [this] { enqueueSelected(); });

    on(ActionId::PlaybackPlayPause, [this] { playback_->playPause(); });
    on(ActionId::PlaybackStop, [this] { playback_->stop(); });
    on(ActionId::PlaybackNext, [this] { playback_->next(); });
    on(ActionId::PlaybackPrevious, [this] { playback_->previous(); });

    const auto setRepeat = [this](RepeatMode mode) {
        playlist_.setRepeat(mode);
        settings_.setRepeatMode(static_cast<int>(mode));
    };
    on(ActionId::OrderRepeatNone, [setRepeat] { setRepeat(RepeatMode::None); });
    on(ActionId::OrderRepeatOne, [setRepeat] { setRepeat(RepeatMode::One); });
    on(ActionId::OrderRepeatAlbum, [setRepeat] { setRepeat(RepeatMode::Album); });
    on(ActionId::OrderRepeatAll, [setRepeat] { setRepeat(RepeatMode::All); });

    const auto setShuffle = [this](ShuffleMode mode) {
        playlist_.setShuffle(mode);
        settings_.setShuffleMode(static_cast<int>(mode));
    };
    on(ActionId::OrderShuffleOff, [setShuffle] { setShuffle(ShuffleMode::Off); });
    on(ActionId::OrderShuffleAlbums, [setShuffle] { setShuffle(ShuffleMode::Albums); });
    on(ActionId::OrderShuffleAll, [setShuffle] { setShuffle(ShuffleMode::All); });

    connect(view_, &QTableView::doubleClicked, this, &MainWindow::onRowActivated);

    connect(playback_.get(), &PlaybackController::positionChanged, this,
            &MainWindow::onPositionChanged);
    connect(playback_.get(), &PlaybackController::currentTrackChanged, this,
            &MainWindow::onCurrentTrackChanged);
    connect(playback_.get(), &PlaybackController::playbackStateChanged, this,
            &MainWindow::onPlaybackStateChanged);
    connect(playback_.get(), &PlaybackController::playbackFailed, this,
            [this](TrackId, const QString& reason) {
                statusBar()->showMessage(reason, 8000);
            });

    // Scrubbing: ignore position updates while the handle is held, or it fights
    // the cursor, and only seek on release rather than on every pixel.
    connect(seekBar_, &QSlider::sliderPressed, this, [this] { scrubbing_ = true; });
    connect(seekBar_, &QSlider::sliderReleased, this, [this] {
        scrubbing_ = false;
        playback_->seek(static_cast<double>(seekBar_->value()) / kSeekScale);
    });

    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        playback_->setVolume(static_cast<double>(value) / 100.0);
    });

    connect(filter_, &QLineEdit::textChanged, proxy_, &PlaylistProxyModel::setFilterText);

    connect(model_, &PlaylistModel::filesDropped, this,
            [this](const QList<QUrl>& urls, int row) { addUrls(urls, row); });
}

// --- commands -----------------------------------------------------------

void MainWindow::openFiles() {
    // The filter comes from the registry rather than a hand-kept list, so a new
    // codec appears in the dialog with no further edit. Cog generates the same
    // thing into CFBundleDocumentTypes at build time.
    QString patterns;
    for (const std::string& extension : registry_.allExtensions()) {
        patterns += QStringLiteral("*.%1 ").arg(QString::fromStdString(extension));
    }

    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, tr("Open Files"), {},
        tr("Audio files (%1);;All files (*)").arg(patterns.trimmed()));

    QList<QUrl> urls;
    urls.reserve(chosen.size());
    for (const QString& path : chosen) {
        urls.append(QUrl::fromLocalFile(path));
    }
    addUrls(urls);
}

void MainWindow::openFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Folder"));
    if (folder.isEmpty()) {
        return;
    }
    addUrls({QUrl::fromLocalFile(folder)});
}

void MainWindow::openUrls(const QList<QUrl>& urls) { addUrls(urls); }

void MainWindow::addUrls(const QList<QUrl>& urls, int atRow) {
    if (urls.isEmpty()) {
        return;
    }

    std::vector<Url> inputs;
    inputs.reserve(static_cast<std::size_t>(urls.size()));
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            inputs.push_back(Url::fromLocalPath(url.toLocalFile().toStdString()));
        } else if (auto parsed = Url::parse(url.toString().toStdString())) {
            inputs.push_back(*parsed);
        }
    }
    if (inputs.empty()) {
        return;
    }

    // Synchronous for now, with a modal progress dialog. A scan of a large
    // folder belongs on a worker thread -- Scanner is already cancellable and
    // reports progress for exactly that -- but a background scan needs the
    // playlist mutation marshalled back, and doing that half-way is worse than
    // doing it plainly.
    QProgressDialog progress(tr("Reading files…"), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);

    Scanner scanner{registry_};
    scanner.setCache(&cache_);
    scanner.setProgressCallback([&progress](std::size_t done, std::size_t total) {
        progress.setMaximum(static_cast<int>(total));
        progress.setValue(static_cast<int>(done));
        QCoreApplication::processEvents();
    });

    std::vector<PlaylistEntry> entries = scanner.scan(inputs);
    if (entries.empty()) {
        statusBar()->showMessage(tr("Nothing playable was found"), 5000);
        return;
    }

    if (library_) {
        for (PlaylistEntry& entry : entries) {
            static_cast<void>(library_->adoptArtwork(entry));
        }
    }

    const std::size_t where =
        (atRow >= 0) ? static_cast<std::size_t>(atRow) : playlist_.size();
    playlist_.insert(where, std::move(entries));

    statusBar()->showMessage(statusSummary());
}

void MainWindow::removeSelected() {
    std::vector<TrackId> ids;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows()) {
        ids.push_back(static_cast<TrackId>(
            proxy_->data(index, PlaylistModel::TrackIdRole).toULongLong()));
    }
    playlist_.remove(ids);
    statusBar()->showMessage(statusSummary());
}

void MainWindow::enqueueSelected() {
    for (const QModelIndex& index : view_->selectionModel()->selectedRows()) {
        playlist_.enqueue(static_cast<TrackId>(
            proxy_->data(index, PlaylistModel::TrackIdRole).toULongLong()));
    }
}

void MainWindow::onRowActivated(const QModelIndex& index) {
    const auto id = static_cast<TrackId>(
        proxy_->data(index, PlaylistModel::TrackIdRole).toULongLong());
    playback_->playTrack(id);
}

// --- reacting to playback -----------------------------------------------

void MainWindow::onPositionChanged(double seconds, double duration) {
    if (!scrubbing_) {
        seekBar_->setRange(0, static_cast<int>(duration * kSeekScale));
        seekBar_->setValue(static_cast<int>(seconds * kSeekScale));
    }
    clock_->setText(QStringLiteral("%1 / %2").arg(formatClock(seconds),
                                                  formatClock(duration)));
}

void MainWindow::onCurrentTrackChanged(TrackId id) {
    model_->setCurrentTrack(id);

    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        nowPlaying_->clear();
        setWindowTitle(QStringLiteral("XPCog"));
        seekBar_->setValue(0);
        return;
    }

    const QString display = QString::fromStdString(entry->display());
    nowPlaying_->setText(display);
    setWindowTitle(QStringLiteral("%1 — XPCog").arg(display));

    // Follow the playing track, but only when it is visible in the current
    // sort and filter: scrolling to a row the user has filtered out would jump
    // the view somewhere they did not ask to be.
    const int row = model_->rowForTrack(id);
    if (row >= 0) {
        const QModelIndex source = model_->index(row, 0);
        const QModelIndex mapped = proxy_->mapFromSource(source);
        if (mapped.isValid()) {
            view_->scrollTo(mapped, QAbstractItemView::EnsureVisible);
        }
    }
}

void MainWindow::onPlaybackStateChanged(bool playing, bool paused) {
    seekBar_->setEnabled(playing);
    if (QAction* command = actions_->action(ActionId::PlaybackPlayPause);
        command != nullptr) {
        command->setText((playing && !paused) ? tr("&Pause") : tr("&Play"));
    }
}

QString MainWindow::statusSummary() const {
    double total = 0.0;
    for (const PlaylistEntry& entry : playlist_.entries()) {
        total += entry.duration();
    }
    return tr("%n track(s)", nullptr, static_cast<int>(playlist_.size())) +
           QStringLiteral(" — ") + formatClock(total);
}

// --- window events ------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event) {
    if (library_ && !library_->savePlaylist(playlist_)) {
        // Worth saying, but not worth refusing to close over.
        statusBar()->showMessage(
            tr("Could not save the playlist: %1")
                .arg(QString::fromStdString(library_->lastError())));
    }
    settings_.sync();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        addUrls(event->mimeData()->urls());
        event->acceptProposedAction();
    }
}

}  // namespace xpcog::app
