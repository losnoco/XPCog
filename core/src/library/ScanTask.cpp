#include "xpcog/core/library/ScanTask.hpp"

#include <memory>
#include <utility>

namespace xpcog {

ScanTask::ScanTask(const PluginRegistry& registry, PluginCache* cache,
                   std::vector<Url> inputs, Dispatcher dispatch,
                   Scanner::Options options)
    : scanner_(registry, options),
      inputs_(std::move(inputs)),
      dispatch_(std::move(dispatch)) {
    scanner_.setCache(cache);
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
    // Throttled: Scanner calls back once per file, and one hop per file over a
    // ten-thousand-track folder floods the interface's queue with updates nobody
    // can read. Every 32 keeps the bar moving smoothly at a fraction of the cost.
    constexpr std::size_t kEvery = 32;

    scanner_.setProgressCallback([this](std::size_t done, std::size_t total) {
        if (done % kEvery != 0 && done != total) {
            return;
        }
        const int doneCount  = static_cast<int>(done);
        const int totalCount = static_cast<int>(total);
        dispatch_([this, doneCount, totalCount] { progress.publish(doneCount, totalCount); });
    });

    std::vector<PlaylistEntry> entries   = scanner_.scan(inputs_);
    const bool                 cancelled = scanner_.cancelled();

    // Through a shared_ptr rather than captured by value: the callable is stored
    // until the interface's loop gets to it, and a std::function must be
    // copyable, which would copy the whole entry vector -- tens of thousands of
    // strings on a real library -- at least once more than necessary.
    auto payload = std::make_shared<std::vector<PlaylistEntry>>(std::move(entries));

    // Dispatched, so the entries are handed over on the interface's thread and
    // everything downstream -- the playlist, the view, the undo stack -- stays
    // single-threaded.
    dispatch_([this, payload, cancelled] { finished.publish(*payload, cancelled); });
}

}  // namespace xpcog
