#include "xpcog/core/library/ScanTask.hpp"

#include <chrono>
#include <memory>
#include <utility>

namespace xpcog {
namespace {

/// How far apart two activity reports have to be to be worth showing. Roughly
/// seven a second: fast enough to look alive, slow enough to read a name off.
///
/// At namespace scope because a lambda cannot use a local one without capturing
/// it -- `operator<` on two durations takes them by reference, which is an
/// odr-use.
constexpr auto kMinimumGap = std::chrono::milliseconds(140);

}  // namespace

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

    // The same flood, thinned the other way. The bar wants a smooth count, so
    // every 32nd is right for it; the text wants to be readable, and readable is
    // a clock question rather than a count one -- thirty-two files can go past in
    // a millisecond during the walk and take a minute during the read.
    scanner_.setActivityCallback([this, last = std::chrono::steady_clock::time_point{}](
                                     Scanner::Phase phase, const Url& url,
                                     std::size_t done, std::size_t total) mutable {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < kMinimumGap) {
            return;
        }
        last = now;

        const Activity item{phase, url, static_cast<int>(done), static_cast<int>(total)};
        dispatch_([this, item] {
            activity.publish(item);
            // And the busy indicator this class's header has always promised for
            // the first pass. Nothing published it before: Scanner counted only
            // while reading, so the bar sat still for the whole of a walk over a
            // large tree -- the part of a scan that most looks like a hang.
            if (item.phase == Scanner::Phase::Finding) {
                progress.publish(item.done, 0);
            }
        });
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
