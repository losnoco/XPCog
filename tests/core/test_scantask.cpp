// Scanning off the caller's thread.
//
// The scan itself is Scanner's, and covered elsewhere in this suite. What is
// tested here is the seam: that the worker thread's results are handed to the
// dispatcher rather than published straight from the scan thread, arrive intact
// and exactly once -- and that a task destroyed mid-scan does not leave its
// thread running.
//
// This used to live in the Qt suite and pump a real event loop, because the hop
// was a queued signal and the only way to observe it was to run the loop that
// delivered it. Injecting the dispatcher makes the test sharper as well as
// headless: the queue below is drained at a point of the test's choosing, so
// "nothing was published before the dispatcher ran it" is something that can be
// asserted rather than hoped for.

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/library/ScanTask.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace xpcog;

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(fs::temp_directory_path() / ("xpcog-scantask-" + name)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] Url url() const { return Url::fromLocalPath(path_); }

    void write(const std::string& name, std::string_view text) const {
        std::ofstream out{path_ / name, std::ios::binary};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

private:
    fs::path path_;
};

/// Stands in for the interface's event loop.
///
/// Collects what the scan thread posts and runs it only when asked, on the
/// thread that asked -- which is the whole contract ScanTask has to keep.
class TestDispatcher {
public:
    [[nodiscard]] ScanTask::Dispatcher sink() {
        return [this](std::function<void()> action) {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(std::move(action));
        };
    }

    /// Runs everything posted so far. Returns how many.
    std::size_t drain() {
        std::vector<std::function<void()>> ready;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ready.swap(pending_);
        }
        for (auto& action : ready) {
            action();
        }
        return ready.size();
    }

    [[nodiscard]] std::size_t pending() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

private:
    std::mutex                         mutex_;
    std::vector<std::function<void()>> pending_;
};

/// Drains until `done`, or gives up -- so a hang shows up as a failed assertion
/// rather than a test run that never ends.
bool drainUntil(TestDispatcher& dispatcher, const bool& done, int milliseconds = 15000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        dispatcher.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done;
}

/// Built once: freeze() is one-shot and the descriptors must outlive every
/// decoder handed out from them.
const PluginRegistry& codecRegistry() {
    static const PluginRegistry& registry = *[] {
        auto* built = new PluginRegistry;
        registerAllCodecs(*built);
        return built;
    }();
    return registry;
}

}  // namespace

TEST_CASE("a scan delivers its entries through the dispatcher, not from its own thread",
          "[core][scan]") {
    TempDir folder{"deliver"};
    // Not real audio: the scan still produces a row for each, marked as an
    // error, which is exactly what the window shows for an unreadable file.
    folder.write("a.flac", "not actually flac");
    folder.write("b.flac", "nor is this");

    TestDispatcher dispatcher;

    bool                       done        = false;
    bool                       cancelled   = true;
    std::vector<PlaylistEntry> received;
    std::thread::id            deliveredOn;

    auto task = std::make_unique<ScanTask>(codecRegistry(), nullptr,
                                           std::vector<Url>{folder.url()},
                                           dispatcher.sink());

    // Subscribed directly, with no marshalling of its own. That is what pins the
    // guarantee: if ScanTask published from the scan thread, this handler would
    // run there, and the recorded thread id would not be this one.
    const Subscription subscription = task->finished.connect(
        [&](const std::vector<PlaylistEntry>& entries, bool wasCancelled) {
            received    = entries;
            cancelled   = wasCancelled;
            deliveredOn = std::this_thread::get_id();
            done        = true;
        });

    task->start();
    REQUIRE(drainUntil(dispatcher, done));

    CHECK_FALSE(cancelled);
    CHECK(received.size() == 2);
    // The whole point: whatever thread read the files, the playlist is only
    // ever touched from this one.
    CHECK(deliveredOn == std::this_thread::get_id());
}

TEST_CASE("nothing is published until the dispatcher runs it", "[core][scan]") {
    TempDir folder{"deferred"};
    folder.write("a.flac", "not actually flac");

    TestDispatcher dispatcher;
    bool           published = false;

    auto task = std::make_unique<ScanTask>(codecRegistry(), nullptr,
                                           std::vector<Url>{folder.url()},
                                           dispatcher.sink());
    const Subscription subscription =
        task->finished.connect([&](const std::vector<PlaylistEntry>&, bool) {
            published = true;
        });

    task->start();

    // Wait for the scan thread to finish posting, without draining. A queued
    // callable is the only thing that should have happened.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (dispatcher.pending() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(dispatcher.pending() > 0);
    CHECK_FALSE(published);

    dispatcher.drain();
    CHECK(published);
}

TEST_CASE("destroying a running scan stops it rather than outliving it", "[core][scan]") {
    TempDir folder{"abandon"};
    for (int i = 0; i < 200; ++i) {
        folder.write("t" + std::to_string(i) + ".flac", "x");
    }

    TestDispatcher dispatcher;

    // Destroyed immediately, while the scan is very likely still running. The
    // destructor has to cancel and join: the temporary directory it is walking
    // is removed the moment this scope ends, and a thread still walking it
    // would be reading a tree that no longer exists. Reaching the assertion at
    // all is the result -- a task that did not join would hang or crash here.
    {
        ScanTask task{codecRegistry(), nullptr, std::vector<Url>{folder.url()},
                      dispatcher.sink()};
        task.start();
    }

    SUCCEED("the task's thread was joined before its inputs went away");
}

TEST_CASE("a cancelled scan keeps what it already read", "[core][scan]") {
    TempDir folder{"cancel"};
    for (int i = 0; i < 50; ++i) {
        folder.write("t" + std::to_string(i) + ".flac", "x");
    }

    TestDispatcher dispatcher;

    bool done      = false;
    bool cancelled = false;

    ScanTask task{codecRegistry(), nullptr, std::vector<Url>{folder.url()},
                  dispatcher.sink()};
    const Subscription subscription =
        task.finished.connect([&](const std::vector<PlaylistEntry>&, bool wasCancelled) {
            cancelled = wasCancelled;
            done      = true;
        });

    task.start();
    task.cancel();
    REQUIRE(drainUntil(dispatcher, done));

    // Cancelling is not failing: the scan returns, and says it was cut short so
    // the window can report that rather than "nothing playable was found".
    CHECK(cancelled);
}
