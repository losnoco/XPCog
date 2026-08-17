// A folder scan, off the GUI thread.
//
// Scanner opens every file it finds and reads its tags. On a music library that
// is minutes of work, and doing it in the event handler freezes the window for
// the duration -- the modal progress dialog it used to show was honest about
// that, not a fix for it.
//
// The scan therefore runs on its own thread and reports back through queued
// signals. Nothing else moves: Scanner stays synchronous and Qt-free, the
// playlist is still only ever touched on the GUI thread, and this class is the
// single seam between the two.
//
// One task runs at a time. The shared PluginCache is not synchronised, and two
// scans racing on it is a real bug rather than a theoretical one; the window
// queues further requests behind the running task instead.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <QObject>

#include <atomic>
#include <thread>
#include <vector>

namespace xpcog::app {

class ScanTask : public QObject {
    Q_OBJECT

public:
    ScanTask(const PluginRegistry& registry, PluginCache* cache,
             std::vector<Url> inputs, QObject* parent = nullptr);

    /// Cancels and joins. A task destroyed mid-scan does not outlive its
    /// thread, so the registry and cache it borrowed cannot go first.
    ~ScanTask() override;

    ScanTask(const ScanTask&)            = delete;
    ScanTask& operator=(const ScanTask&) = delete;

    void start();

    /// Asks the scan to stop. It finishes with whatever it has, so cancelling
    /// halfway through a large folder keeps the tracks already read rather than
    /// throwing the work away.
    void cancel();

signals:
    /// `total` is zero until the expansion pass has counted the files, which is
    /// itself the slow part on a large tree -- so the first phase has no
    /// meaningful percentage and the window shows a busy indicator for it.
    void progress(int done, int total);

    /// Emitted on the GUI thread once, whether the scan completed or was
    /// cancelled. The task deletes itself afterwards.
    void finished(const std::vector<xpcog::PlaylistEntry>& entries, bool cancelled);

private:
    void run();

    Scanner          scanner_;
    std::vector<Url> inputs_;
    std::thread      thread_;
};

}  // namespace xpcog::app

Q_DECLARE_METATYPE(std::vector<xpcog::PlaylistEntry>)
