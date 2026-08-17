#include "ScanTask.hpp"

#include <QMetaObject>

#include <utility>

namespace xpcog::app {

ScanTask::ScanTask(const PluginRegistry& registry, PluginCache* cache,
                   std::vector<Url> inputs, QObject* parent)
    : QObject(parent), scanner_(registry), inputs_(std::move(inputs)) {
    scanner_.setCache(cache);
    // Registered here rather than in main(): the signal that needs it is
    // declared in this file, so the two cannot drift apart.
    qRegisterMetaType<std::vector<PlaylistEntry>>("std::vector<xpcog::PlaylistEntry>");
}

ScanTask::~ScanTask() {
    cancel();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ScanTask::cancel() { scanner_.cancel(); }

void ScanTask::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void ScanTask::run() {
    // Throttled: Scanner calls back once per file, and a queued signal per file
    // over a ten-thousand-track folder floods the GUI thread's event queue with
    // updates nobody can read. Every 32 keeps the bar moving smoothly at a
    // fraction of the cost.
    constexpr std::size_t kEvery = 32;

    scanner_.setProgressCallback([this](std::size_t done, std::size_t total) {
        if (done % kEvery != 0 && done != total) {
            return;
        }
        QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                                  Q_ARG(int, static_cast<int>(done)),
                                  Q_ARG(int, static_cast<int>(total)));
    });

    std::vector<PlaylistEntry> entries = scanner_.scan(inputs_);
    const bool                 cancelled = scanner_.cancelled();

    // Queued, so the entries are handed over on the GUI thread and everything
    // downstream -- the playlist, the model, the undo stack -- stays
    // single-threaded.
    QMetaObject::invokeMethod(
        this,
        [this, entries = std::move(entries), cancelled]() mutable {
            emit finished(entries, cancelled);
        },
        Qt::QueuedConnection);
}

}  // namespace xpcog::app
