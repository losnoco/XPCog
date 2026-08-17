#include "MainWindow.hpp"

#include "ActionRegistry.hpp"
#include "windows/AboutDialog.hpp"
#include "windows/MiniWindow.hpp"
#include "FileTree.hpp"
#include "PlaybackController.hpp"
#include "PlaylistCommands.hpp"
#include "PlaylistModel.hpp"
#include "ScanTask.hpp"
#include "SeekSlider.hpp"
#include "SpectrumWidget.hpp"
#include "StatusPresence.hpp"
#include "PreferencesDialog.hpp"

#include "xpcog/core/Version.hpp"
#include "xpcog/core/library/PlaylistFile.hpp"
#include "xpcog/core/library/Scanner.hpp"
#include "xpcog/platform/QSettingsStore.hpp"
#include "xpcog/platform/TaskbarIntegration.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QProgressBar>
#include <QSaveFile>
#include <QSettings>
#include <QDockWidget>
#include <QSplitter>
#include <QSlider>
#include <QStatusBar>
#include <QTableView>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
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
    undo_     = new QUndoStack(this);
    media_    = platform::MediaIntegration::create(this);

    buildUi();
    buildMenus();
    wireUp();

    // After buildMenus(), because it hands out the same QActions the menu bar
    // uses and they have to exist first.
    presence_ = new StatusPresence(*actions_, this, this);

    // winId() realises the native window, which is what an overlay icon needs a
    // handle to. Calls made before the window is shown are harmless -- the taskbar
    // has no button to decorate yet -- and everything that drives this happens on a
    // playback or scan change, which is necessarily later.
    taskbar_ = platform::TaskbarIntegration::create(winId(), this);

    if (library_ && library_->loadPlaylist(playlist_)) {
        statusBar()->showMessage(statusSummary());
    }
    // Restoring the saved playlist is not an edit the user made, so it must not
    // be the first thing Undo offers to take back.
    undo_->clear();

    // Modes come from settings, which hold Cog's integers.
    playlist_.setRepeat(static_cast<RepeatMode>(settings_.RepeatMode()));
    playlist_.setShuffle(static_cast<ShuffleMode>(settings_.ShuffleMode()));
    playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());

    // Window geometry lives in QSettings rather than settings.def: it is this
    // application's own UI state, not a Cog setting to stay compatible with.
    const QSettings ui;
    restoreGeometry(ui.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(ui.value(QStringLiteral("window/state")).toByteArray());
    if (auto* split = findChild<QSplitter*>(QStringLiteral("mainSplitter"));
        split != nullptr) {
        split->restoreState(ui.value(QStringLiteral("window/splitter")).toByteArray());
    }
    if (const QString root = ui.value(QStringLiteral("fileTree/root")).toString();
        !root.isEmpty()) {
        tree_->setRootPath(root);
    }

    // After restoreState(), which will happily bring back a hidden toolbar
    // saved before it became unhideable.
    transport_->setVisible(true);

    // Cog restores mini mode at launch from the same key. Through the action, so
    // the menu tick agrees with the window that is actually on screen -- and only
    // when it is on, because toggling a checkable action to the state it already
    // holds emits nothing and would leave the mini window unbuilt.
    if (settings_.MiniMode()) {
        if (QAction* command = actions_->action(ActionId::ViewMiniPlayer);
            command != nullptr) {
            command->setChecked(true);
        }
    }
}

QMenu* MainWindow::createPopupMenu() {
    QMenu* menu = QMainWindow::createPopupMenu();
    if (menu != nullptr) {
        menu->removeAction(transport_->toggleViewAction());
        // Nothing left to offer once the transport is out, until docks arrive.
        if (menu->isEmpty()) {
            delete menu;
            return nullptr;
        }
    }
    return menu;
}

MainWindow::~MainWindow() {
    // The scan borrows the registry and the PluginCache, and the cache is a
    // member of this window. QObject deletes its children only after every
    // member is already gone, so leaving the task to that would let its thread
    // outlive what it is reading from. ~ScanTask cancels and joins.
    delete scan_;
    scan_ = nullptr;
}

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
    // setSortingEnabled(true) sorts immediately by the header's current
    // section, which defaults to 0 -- the status column, where every row ties.
    // Clearing the indicator opens the window in playlist order and shows no
    // sort arrow, which is the truth.
    view_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);

    tree_ = new FileTree(registry_, this);

    // The splitter, not a dock: Cog's file tree is a fixed pane of the window
    // and behaves as one. Its position is restored below.
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(tree_);
    split->addWidget(view_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 840});
    split->setObjectName(QStringLiteral("mainSplitter"));
    setCentralWidget(split);

    // A dock rather than a pane of the splitter: the spectrum is optional and wants
    // the full width, and a dock is what already gets its visibility and size saved
    // by saveState() below. (Cog puts it in a separate window; a dock is the same
    // thing with the window management done for us, and it can still be torn off.)
    spectrum_ = new SpectrumWidget(playback_->tap(), this);
    auto* spectrumDock = new QDockWidget(tr("Spectrum"), this);
    spectrumDock->setObjectName(QStringLiteral("spectrumDock"));
    spectrumDock->setWidget(spectrum_);
    spectrumDock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, spectrumDock);
    spectrumDock_ = spectrumDock;
    spectrum_->applySettings(settings_);

    transport_ = addToolBar(tr("Transport"));
    // saveState() identifies toolbars by objectName and warns without one --
    // and, more to the point, silently fails to match them up again on
    // restoreState(), so the transport's place in the layout is not restored.
    transport_->setObjectName(QStringLiteral("transportToolBar"));
    transport_->setMovable(false);
    transport_->setFloatable(false);
    // Right-clicking the toolbar itself must not offer to hide it either.
    transport_->setContextMenuPolicy(Qt::PreventContextMenu);
    // ...and it must not appear as a hideable item in any menu built from
    // toggleViewAction(), which is the other route to the same dead end.
    transport_->toggleViewAction()->setVisible(false);
    auto* transport = transport_;

    seekBar_ = new SeekSlider(transport);
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

    // In the status bar rather than a modal dialog: the scan runs on its own
    // thread now, so there is nothing to block for. The window stays usable --
    // including playing what has already been added -- while a large folder is
    // still being read.
    scanBar_ = new QProgressBar(this);
    scanBar_->setMaximumWidth(140);
    scanBar_->setTextVisible(false);
    scanBar_->hide();
    statusBar()->addPermanentWidget(scanBar_);

    // Losing the modal dialog also loses its Cancel button, and a scan of a
    // mistakenly-dropped drive needs a way out that is not quitting.
    scanCancel_ = new QToolButton(this);
    scanCancel_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    scanCancel_->setAutoRaise(true);
    scanCancel_->setToolTip(tr("Stop reading files"));
    scanCancel_->hide();
    statusBar()->addPermanentWidget(scanCancel_);

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
    if (auto* transport = transport_; transport != nullptr) {
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
    on(ActionId::FileSavePlaylist, [this] { savePlaylistAs(); });
    on(ActionId::FilePreferences, [this] { showPreferences(); });
    on(ActionId::HelpAbout, [this] {
        AboutDialog dialog{registry_, this};
        dialog.exec();
    });
    on(ActionId::HelpAboutQt, [] { QApplication::aboutQt(); });
    on(ActionId::FileQuit, [this] { requestQuit(); });

    if (QAction* command = actions_->action(ActionId::ViewFileTree); command != nullptr) {
        connect(command, &QAction::toggled, tree_, &QWidget::setVisible);
    }

    // `enabled`, not `on`: wireUp()'s helper for connecting an ActionId to a slot
    // is called `on`, and a lambda parameter of the same name shadows it. GCC's
    // -Wshadow catches it; MSVC does not, which is why it reached CI.
    if (QAction* command = actions_->action(ActionId::ViewMiniPlayer); command != nullptr) {
        connect(command, &QAction::toggled, this,
                [this](bool enabled) { setMiniMode(enabled); });
    }

    // Both directions, because a dock can also be closed by its own title bar and
    // the menu item has to follow. No loop: setChecked only emits when the state
    // actually changes.

    if (QAction* command = actions_->action(ActionId::ViewSpectrum); command != nullptr) {
        connect(command, &QAction::toggled, spectrumDock_, &QWidget::setVisible);
        connect(spectrumDock_, &QDockWidget::visibilityChanged, command,
                &QAction::setChecked);
    }

    connect(tree_, &FileTree::activated, this,
            [this](const QList<QUrl>& urls) { addUrls(urls); });
    connect(tree_, &FileTree::addRequested, this,
            [this](const QList<QUrl>& urls) { addUrls(urls); });
    on(ActionId::EditSelectAll, [this] { view_->selectAll(); });
    on(ActionId::EditRemove, [this] { removeSelected(); });
    on(ActionId::PlaybackEnqueue, [this] { enqueueSelected(); });

    on(ActionId::EditUndo, [this] { undo_->undo(); });
    on(ActionId::EditRedo, [this] { undo_->redo(); });
    on(ActionId::EditRandomize, [this] {
        if (playlist_.size() > 1) {
            undo_->push(new RandomizeCommand(playlist_));
        }
    });
    connect(undo_, &QUndoStack::indexChanged, this, [this] { refreshUndoActions(); });
    refreshUndoActions();

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

    // Seek on release rather than on every pixel: seeking per mouse-move would
    // ask the decoder to jump dozens of times across one drag.
    connect(seekBar_, &SeekSlider::seekRequested, this, [this](double seconds) {
        playback_->seek(seconds / kSeekScale);
    });
    // The clock follows the handle during the drag, so there is something to
    // aim with before letting go.
    connect(seekBar_, &SeekSlider::scrubbed, this, [this](double seconds) {
        clock_->setText(QStringLiteral("%1 / %2")
                            .arg(formatClock(seconds / kSeekScale),
                                 formatClock(duration_)));
    });

    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        playback_->setVolume(static_cast<double>(value) / 100.0);
        // Also to the OS, which on Linux publishes the volume as a property a
        // desktop panel shows its own slider for. Without this the panel's slider
        // sits wherever it last put it while the audio does something else.
        //
        // No loop: an MPRIS write arrives as volumeRequested, which moves this
        // slider, which lands here -- and setVolume only stores the value and
        // announces it, so the round trip stops.
        media_->setVolume(static_cast<float>(value) / 100.0F);
    });

    connect(filter_, &QLineEdit::textChanged, proxy_, &PlaylistProxyModel::setFilterText);

    // The media keys and the Now Playing widget drive the same commands the
    // buttons do, rather than reaching into the engine separately.
    connect(media_, &platform::MediaIntegration::playPauseRequested, this,
            [this] { playback_->playPause(); });
    connect(media_, &platform::MediaIntegration::playRequested, this, [this] {
        if (!playback_->playing() || playback_->paused()) {
            playback_->playPause();
        }
    });
    connect(media_, &platform::MediaIntegration::pauseRequested, this, [this] {
        if (playback_->playing() && !playback_->paused()) {
            playback_->playPause();
        }
    });
    connect(media_, &platform::MediaIntegration::stopRequested, this,
            [this] { playback_->stop(); });
    connect(media_, &platform::MediaIntegration::nextRequested, this,
            [this] { playback_->next(); });
    connect(media_, &platform::MediaIntegration::previousRequested, this,
            [this] { playback_->previous(); });
    connect(media_, &platform::MediaIntegration::seekRequested, this,
            [this](double seconds) { playback_->seek(seconds); });

    // MPRIS only, on Linux. The other two platforms never emit these, so there
    // is nothing to guard: a signal that is never sent costs a connection.
    connect(media_, &platform::MediaIntegration::raiseRequested, this,
            [this] { raiseWindow(this); });
    connect(media_, &platform::MediaIntegration::quitRequested, this,
            [] { QApplication::quit(); });
    connect(media_, &platform::MediaIntegration::volumeRequested, this,
            [this](float gain) {
                // Through the slider rather than straight to the engine, so the
                // panel and the window cannot end up showing different volumes.
                // The slider's own signal is what then reaches playback_.
                volume_->setValue(static_cast<int>(std::lround(gain * 100.0F)));
            });
    connect(media_, &platform::MediaIntegration::openUrlRequested, this,
            [this](const QUrl& url) { openUrls({url}); });

    connect(scanCancel_, &QToolButton::clicked, this, [this] {
        // Everything not yet started goes too: cancelling one folder of a
        // dropped batch and then watching the next one start is not what the
        // button looks like it does.
        pendingScans_.clear();
        if (scan_ != nullptr) {
            scan_->cancel();
        }
    });

    connect(model_, &PlaylistModel::filesDropped, this,
            [this](const QList<QUrl>& urls, int row) { addUrls(urls, row); });

    connect(model_, &PlaylistModel::reorderRequested, this,
            [this](const std::vector<TrackId>& order) {
                undo_->push(new ReorderCommand(playlist_, order, tr("Move Tracks")));
            });
}

void MainWindow::refreshUndoActions() {
    const auto label = [](const QString& base, const QString& what) {
        return what.isEmpty() ? base : QStringLiteral("%1 %2").arg(base, what);
    };

    if (QAction* command = actions_->action(ActionId::EditUndo); command != nullptr) {
        command->setEnabled(undo_->canUndo());
        command->setText(label(tr("&Undo"), undo_->undoText()));
    }
    if (QAction* command = actions_->action(ActionId::EditRedo); command != nullptr) {
        command->setEnabled(undo_->canRedo());
        command->setText(label(tr("&Redo"), undo_->redoText()));
    }
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

void MainWindow::showPreferences() {
    PreferencesDialog dialog{settings_, this};
    connect(&dialog, &PreferencesDialog::settingChanged, this, [this](const QString& key) {
        // Most settings are read live by the engine. The ones the playlist owns
        // have to be pushed across, since Playlist deliberately does not read
        // settings itself.
        if (key == QLatin1String("alwaysStopAfterCurrent")) {
            playlist_.setStopAfterCurrent(settings_.AlwaysStopAfterCurrent());
        }
        // The equaliser is push, not poll: re-reading 32 keys per chunk to notice
        // a slider move would cost more than the filter itself. Every one of
        // those keys begins "eq".
        if (key.startsWith(QLatin1String("eq"))) {
            playback_->reloadDsp();
        }
        // Push for the same reason, and immediate for a better one: every setting
        // here is about what the display *looks* like, and a colour you have to
        // restart to see is not a colour picker, it is a form.
        if (key.startsWith(QLatin1String("spectrum"))) {
            spectrum_->applySettings(settings_);
        }
    });
    dialog.exec();
    settings_.sync();
}

void MainWindow::savePlaylistAs() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Playlist"), {},
        tr("M3U playlist (*.m3u);;PLS playlist (*.pls);;XSPF playlist (*.xspf);;"
           "Cog XML playlist (*.xml)"));
    if (path.isEmpty()) {
        return;
    }

    const Url destination = Url::fromLocalPath(path.toStdString());
    const auto format = playlistFormatForExtension(destination.extension());
    if (!format) {
        QMessageBox::warning(this, tr("Save Playlist"),
                             tr("Unrecognised playlist extension."));
        return;
    }

    // Queue positions are stored as indices into the entries written, which is
    // what the Cog XML reader expects back.
    std::vector<std::size_t> queue;
    for (const TrackId id : playlist_.queue()) {
        if (const auto index = playlist_.indexOf(id)) {
            queue.push_back(*index);
        }
    }

    const std::string text =
        writePlaylist(*format, playlist_.entries(), queue, destination);

    // QSaveFile, so a failure part-way leaves the previous playlist intact
    // rather than a truncated one.
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
        file.write(text.data(), static_cast<qint64>(text.size())) < 0 ||
        !file.commit()) {
        QMessageBox::warning(this, tr("Save Playlist"),
                             tr("Could not write %1").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Saved %1").arg(path), 5000);
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

    pendingScans_.push_back(ScanRequest{std::move(inputs), atRow});
    pumpScanQueue();
}

void MainWindow::pumpScanQueue() {
    if (scan_ != nullptr || pendingScans_.empty()) {
        return;
    }

    ScanRequest request = std::move(pendingScans_.front());
    pendingScans_.erase(pendingScans_.begin());

    scan_ = new ScanTask(registry_, &cache_, std::move(request.inputs), this);

    connect(scan_, &ScanTask::progress, this, [this](int done, int total) {
        // A zero maximum is Qt's busy indicator, which is the truthful display
        // while the expansion pass is still counting.
        scanBar_->setMaximum(total);
        scanBar_->setValue(done);

        // And the same number on the taskbar button, which is what Cog puts on its
        // Dock tile -- its progress bar tracks PlaylistLoader, not the seek
        // position. Left alone while the total is still zero: a bar sitting at zero
        // reads as stalled, where no bar reads as "not started", which is the truth.
        if (total > 0) {
            taskbar_->setProgress(static_cast<double>(done) /
                                  static_cast<double>(total));
        }
    });

    const int atRow = request.atRow;
    connect(scan_, &ScanTask::finished, this,
            [this, atRow](const std::vector<PlaylistEntry>& entries, bool cancelled) {
                // The task owns the thread it is still returning from, so it
                // cannot be deleted from inside its own signal.
                scan_->deleteLater();
                scan_ = nullptr;

                scanBar_->hide();
                scanCancel_->hide();
                taskbar_->clearProgress();
                addScannedEntries(entries, atRow, cancelled);
                pumpScanQueue();
            });

    scanBar_->setRange(0, 0);
    scanBar_->show();
    scanCancel_->show();
    statusBar()->showMessage(tr("Reading files…"));
    scan_->start();
}

void MainWindow::addScannedEntries(std::vector<PlaylistEntry> entries, int atRow,
                                   bool cancelled) {
    if (entries.empty()) {
        statusBar()->showMessage(cancelled ? tr("Cancelled")
                                           : tr("Nothing playable was found"),
                                 5000);
        return;
    }

    if (library_) {
        for (PlaylistEntry& entry : entries) {
            static_cast<void>(library_->adoptArtwork(entry));
        }
    }

    // The row a drop targeted may no longer exist: the scan took time and the
    // user could have edited the playlist meanwhile. insert() clamps, so this
    // lands at the end rather than nowhere.
    const std::size_t where =
        (atRow >= 0) ? static_cast<std::size_t>(atRow) : playlist_.size();
    const auto count = static_cast<int>(entries.size());
    undo_->push(new InsertTracksCommand(playlist_, where, std::move(entries),
                                        tr("Add %n Track(s)", nullptr, count)));

    statusBar()->showMessage(statusSummary());
}

void MainWindow::removeSelected() {
    std::vector<TrackId> ids;
    for (const QModelIndex& index : view_->selectionModel()->selectedRows()) {
        ids.push_back(static_cast<TrackId>(
            proxy_->data(index, PlaylistModel::TrackIdRole).toULongLong()));
    }
    if (ids.empty()) {
        return;
    }
    undo_->push(new RemoveTracksCommand(
        playlist_, ids, tr("Remove %n Track(s)", nullptr, static_cast<int>(ids.size()))));
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
    duration_ = duration;

    // Before the early return below: that one is about *this* window's slider
    // being dragged, which says nothing about the mini player's. The mini window
    // guards its own the same way.
    if (mini_ != nullptr) {
        mini_->setPosition(seconds, duration);
    }

    if (seekBar_->scrubbing()) {
        return;  // the handle belongs to the cursor until it is let go
    }

    // Only when it actually changes: setRange on a slider whose handle the user
    // is about to grab causes a visible twitch, and it fires valueChanged.
    const int span = static_cast<int>(duration * kSeekScale);
    if (seekBar_->maximum() != span) {
        seekBar_->setRange(0, span);
    }
    seekBar_->setValue(static_cast<int>(seconds * kSeekScale));

    clock_->setText(QStringLiteral("%1 / %2").arg(formatClock(seconds),
                                                  formatClock(duration)));

    // Not every tick. The OS extrapolates from the rate it was given, so it is
    // already counting correctly between updates; this only has to correct it
    // after a seam or a seek. Also catches a seek jumping backwards.
    constexpr double kResyncSeconds = 5.0;
    if (mediaPosition_ < 0.0 || std::abs(seconds - mediaPosition_) >= kResyncSeconds) {
        media_->setPlaybackState(playback_->playing(), playback_->paused(), seconds);
        mediaPosition_ = seconds;
    }
}

void MainWindow::publishNowPlaying(TrackId id) {
    mediaPosition_ = -1.0;

    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        media_->clear();
        return;
    }

    platform::NowPlayingInfo info;
    info.title    = QString::fromStdString(entry->title());
    info.artist   = QString::fromStdString(entry->artist);
    info.album    = QString::fromStdString(entry->album);
    info.duration = entry->duration();
    info.position = playback_->position();

    // Artwork is content-addressed in the library rather than carried on the
    // entry, so this is the one place that can resolve it.
    if (library_ && !entry->artHash.empty()) {
        // Not `data`: QWidget has a member of that name, so this shadowed it --
        // MSVC's C4458 and GCC's -Wshadow both said so.
        const std::vector<std::byte> artBytes = library_->artwork(entry->artHash);
        if (!artBytes.empty()) {
            info.artwork.loadFromData(
                QByteArray(reinterpret_cast<const char*>(artBytes.data()),
                           static_cast<qsizetype>(artBytes.size())));
        }
    }

    media_->setNowPlaying(info);
}

void MainWindow::onCurrentTrackChanged(TrackId id) {
    model_->setCurrentTrack(id);
    publishNowPlaying(id);

    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        nowPlaying_->clear();
        setWindowTitle(QStringLiteral("XPCog"));
        seekBar_->setValue(0);
        presence_->clear();
        if (mini_ != nullptr) {
            mini_->setNowPlaying(QString{}, QString{});
        }
        return;
    }

    const QString display = QString::fromStdString(entry->display());
    nowPlaying_->setText(display);
    setWindowTitle(QStringLiteral("%1 — XPCog").arg(display));
    presence_->setNowPlaying(QString::fromStdString(entry->title()),
                             QString::fromStdString(entry->artist));
    if (mini_ != nullptr) {
        mini_->setNowPlaying(QString::fromStdString(entry->title()),
                             QString::fromStdString(entry->artist));
    }

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

    presence_->setPlaybackState(playing, paused);
    taskbar_->setPlaybackState(playing, paused);
    if (mini_ != nullptr) {
        mini_->setPlaybackState(playing, paused);
    }

    // The band table depends on Nyquist, so the rate has to arrive before the first
    // frame is analysed -- and it is only knowable once a device is open.
    if (playing) {
        spectrum_->setSampleRate(playback_->sampleRate());
    }
    // Paused counts as inactive: the tap stops being written, so the display would
    // otherwise sit on whatever the last window held.
    spectrum_->setActive(playing && !paused);

    if (!playing) {
        media_->clear();
        mediaPosition_ = -1.0;
        return;
    }
    // Always on a state change, and force the next tick to push as well: a
    // pause has to reach the OS immediately, or its widget keeps counting.
    media_->setPlaybackState(playing, paused, playback_->position());
    mediaPosition_ = -1.0;
}

void MainWindow::setMiniMode(bool mini) {
    if (mini && mini_ == nullptr) {
        mini_ = new MiniWindow(*actions_, *playback_, settings_, this);

        // Closing the mini player returns to the full window rather than quitting.
        // Routed through the action so the menu item's tick follows, which then
        // calls back into here -- setChecked only emits on an actual change, so it
        // settles rather than looping.
        connect(mini_, &MiniWindow::dismissed, this, [this] {
            if (QAction* command = actions_->action(ActionId::ViewMiniPlayer);
                command != nullptr) {
                command->setChecked(false);
            }
        });
        connect(mini_, &MiniWindow::volumeChanged, this, [this](double linear) {
            const QSignalBlocker blocker(volume_);
            volume_->setValue(static_cast<int>(std::lround(linear * 100.0)));
        });

        // Seed it with what is playing, since it was not around when that was
        // announced.
        const PlaylistEntry* entry = playlist_.find(playback_->currentTrack());
        if (entry != nullptr) {
            mini_->setNowPlaying(QString::fromStdString(entry->title()),
                                 QString::fromStdString(entry->artist));
        }
        mini_->setPlaybackState(playback_->playing(), playback_->paused());
        mini_->setPosition(playback_->position(), playback_->duration());
    }

    settings_.setMiniMode(mini);
    if (mini) {
        mini_->refreshVolume();
        mini_->show();
        raiseWindow(mini_);
        hide();
    } else {
        if (mini_ != nullptr) {
            mini_->hide();
        }
        show();
        raiseWindow(this);
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

void MainWindow::persistState() {
    QSettings ui;
    ui.setValue(QStringLiteral("window/geometry"), saveGeometry());
    ui.setValue(QStringLiteral("window/state"), saveState());
    if (auto* split = findChild<QSplitter*>(QStringLiteral("mainSplitter"));
        split != nullptr) {
        ui.setValue(QStringLiteral("window/splitter"), split->saveState());
    }
    ui.setValue(QStringLiteral("fileTree/root"), tree_->rootPath());

    if (library_ && !library_->savePlaylist(playlist_)) {
        // Worth saying, but not worth refusing to close over.
        statusBar()->showMessage(
            tr("Could not save the playlist: %1")
                .arg(QString::fromStdString(library_->lastError())));
    }
    settings_.sync();
}

void MainWindow::requestQuit() {
    // Through close() rather than straight to QApplication::quit(), which exits the
    // event loop *without* closing any window -- so closeEvent never ran and nothing
    // was saved. Quitting from the menu or the tray has therefore always discarded
    // the playlist and the window geometry; only closing with the title bar's button
    // saved them. Nobody noticed because that is how most sessions end.
    //
    // Close to tray makes it impossible to miss: with the close button intercepted,
    // Quit becomes the only way out, so the leak would go from occasional to always.
    quitting_ = true;
    close();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Hidden rather than closed, when asked to be and when there is somewhere to
    // hide *to*. hasTrayIcon() rather than isVisible(): on macOS the presence is the
    // Dock menu and there is no tray icon, so honouring this there would hide the
    // window and leave nothing to click.
    if (!quitting_ && settings_.CloseToTray() && presence_->hasTrayIcon()) {
        // Saved anyway. The session continues, so this is not strictly required --
        // but the user has just made a deliberate gesture, and a machine that goes
        // down while XPCog sits in the tray should not cost them the playlist.
        persistState();
        event->ignore();
        hide();

        // Once per session. A window that vanishes with no explanation is
        // indistinguishable from a crash, and this is the only moment where saying
        // so is useful rather than noise.
        if (!announcedTrayHide_) {
            announcedTrayHide_ = true;
            presence_->showMessage(
                tr("XPCog is still playing"),
                tr("The window is closed, not the player. Click the tray icon to "
                   "bring it back."));
        }
        return;
    }

    persistState();
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
