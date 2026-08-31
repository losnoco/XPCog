// A folder scan, off the caller's thread.
//
// Scanner opens every file it finds and reads its tags. On a music library that
// is minutes of work, and doing it in an event handler freezes the window for the
// duration -- the modal progress dialog it used to show was honest about that,
// not a fix for it.
//
// The scan therefore runs on its own thread and reports back through the
// dispatcher it was given. Nothing else moves: Scanner stays synchronous, the
// playlist is still only ever touched on the interface's thread, and this class
// is the single seam between the two.
//
// In core rather than in the application, and the dispatcher is what makes that
// possible: this used to be a QObject whose queued signals hid the hop. Handing
// the hop in as a parameter means the class knows nothing about what kind of
// interface is on the other end -- and means its test can supply a dispatcher
// that just collects the callables, and drain them at a point of its choosing.
// That is a sharper test than pumping a real event loop and hoping.
//
// One task runs at a time. The shared PluginCache is not synchronised, and two
// scans racing on it is a real bug rather than a theoretical one; the caller
// queues further requests behind the running task instead.

#pragma once

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Signal.hpp"
#include "xpcog/core/Url.hpp"
#include "xpcog/core/library/PlaylistEntry.hpp"
#include "xpcog/core/library/PluginCache.hpp"
#include "xpcog/core/library/Scanner.hpp"

#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace xpcog {

class ScanTask {
public:
    /// Runs a callable on the thread that owns the user interface. The same
    /// shape as platform::Dispatcher, and for the same reason.
    using Dispatcher = std::function<void(std::function<void()>)>;

    /// `options` is the Scanner's, and the caller supplies it because the
    /// answer lives in Settings, which core takes by injection rather than
    /// reaching for -- the same rule the engine follows.
    ScanTask(const PluginRegistry& registry, PluginCache* cache, std::vector<Url> inputs,
             Dispatcher dispatch, Scanner::Options options = {});

    ScanTask(const ScanTask&)            = delete;
    ScanTask& operator=(const ScanTask&) = delete;

    /// Cancels and joins. A task destroyed mid-scan does not outlive its
    /// thread, so the registry and cache it borrowed cannot go first.
    ~ScanTask();

    void start();

    /// Asks the scan to stop. It finishes with whatever it has, so cancelling
    /// halfway through a large folder keeps the tracks already read rather than
    /// throwing the work away.
    void cancel();

    /// `total` is zero until the expansion pass has counted the files, which is
    /// itself the slow part on a large tree -- so the first phase has no
    /// meaningful percentage and the caller shows a busy indicator for it.
    Signal<int, int> progress;

    /// What the scan has in hand right now: which of Scanner's two passes is
    /// running, the item itself, and the counts `progress` carries.
    ///
    /// A struct rather than four signal parameters because the wording is the
    /// caller's business and this is the material it words it from -- core has
    /// no catalogue and cannot say "Reading" in the user's language.
    struct Activity {
        Scanner::Phase phase = Scanner::Phase::Finding;
        Url            url;
        int            done  = 0;
        int            total = 0;
    };

    /// Published as the scan moves, thinned to a handful a second. Scanner
    /// reports every file it touches, which on a folder walk is thousands a
    /// second: a status line changing that fast is a blur rather than
    /// information, and one hop per file would flood the interface's queue.
    Signal<Activity> activity;

    /// Published once, whether the scan completed or was cancelled, on the
    /// interface's thread.
    Signal<std::vector<PlaylistEntry>, bool> finished;

private:
    void run();

    Scanner          scanner_;
    std::vector<Url> inputs_;
    Dispatcher       dispatch_;
    std::thread      thread_;
};

}  // namespace xpcog
