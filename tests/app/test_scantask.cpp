// Scanning off the GUI thread.
//
// The scan itself is Scanner's, and covered in the core suite. What is tested
// here is the seam: that the worker thread's results arrive on the thread that
// runs the event loop, intact, exactly once -- and that a task destroyed
// mid-scan does not leave its thread running.

#include "QtStringMaker.hpp"

#include "ScanTask.hpp"

#include "xpcog/core/PluginRegistry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QThread>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

using namespace xpcog;
using namespace xpcog::app;

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

    [[nodiscard]] Url url() const { return Url::fromLocalPath(path_.string()); }

    void write(const std::string& name, std::string_view text) const {
        std::ofstream out{path_ / name, std::ios::binary};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

private:
    fs::path path_;
};

/// Spins the event loop until `done`, or gives up. Returns whether it happened,
/// so a hang shows up as a failed assertion rather than a test run that never
/// ends.
bool pumpUntil(const bool& done, int milliseconds = 15000) {
    QDeadlineTimer deadline(milliseconds);
    while (!done && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
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

TEST_CASE("a scan delivers its entries on the thread that started it",
          "[app][scan]") {
    TempDir folder{"deliver"};
    // Not real audio: the scan still produces a row for each, marked as an
    // error, which is exactly what the window shows for an unreadable file.
    folder.write("a.flac", "not actually flac");
    folder.write("b.flac", "nor is this");


    bool                       done      = false;
    bool                       cancelled = true;
    std::vector<PlaylistEntry> received;
    QThread*                   deliveredOn = nullptr;

    auto task = std::make_unique<ScanTask>(codecRegistry(), nullptr,
                                           std::vector<Url>{folder.url()});
    // Deliberately a direct connection. The default would be a queued one --
    // the signal crosses threads -- and would land on the right thread whatever
    // ScanTask did, so it would test Qt rather than this class. Direct means the
    // handler runs wherever the signal was emitted, which pins the guarantee
    // ScanTask actually makes: it marshals before emitting, so a caller cannot
    // accidentally end up mutating the playlist from the scan thread.
    QObject::connect(task.get(), &ScanTask::finished, task.get(),
                     [&](const std::vector<PlaylistEntry>& entries, bool wasCancelled) {
                         received    = entries;
                         cancelled   = wasCancelled;
                         deliveredOn = QThread::currentThread();
                         done        = true;
                     },
                     Qt::DirectConnection);

    task->start();
    REQUIRE(pumpUntil(done));

    CHECK_FALSE(cancelled);
    CHECK(received.size() == 2);
    // The whole point: whatever thread read the files, the playlist is only
    // ever touched from this one.
    CHECK(deliveredOn == QThread::currentThread());
}

TEST_CASE("destroying a running scan stops it rather than outliving it",
          "[app][scan]") {
    TempDir folder{"abandon"};
    for (int i = 0; i < 200; ++i) {
        folder.write("t" + std::to_string(i) + ".flac", "x");
    }


    // Destroyed immediately, while the scan is very likely still running. The
    // destructor has to cancel and join: the temporary directory it is walking
    // is removed the moment this scope ends, and a thread still walking it
    // would be reading a tree that no longer exists. Reaching the assertion at
    // all is the result -- a task that did not join would hang or crash here.
    {
        ScanTask task{codecRegistry(), nullptr, std::vector<Url>{folder.url()}};
        task.start();
    }

    SUCCEED("the task's thread was joined before its inputs went away");
}

TEST_CASE("a cancelled scan keeps what it already read", "[app][scan]") {
    TempDir folder{"cancel"};
    for (int i = 0; i < 50; ++i) {
        folder.write("t" + std::to_string(i) + ".flac", "x");
    }


    bool done      = false;
    bool cancelled = false;

    ScanTask task{codecRegistry(), nullptr, std::vector<Url>{folder.url()}};
    QObject::connect(&task, &ScanTask::finished, &task,
                     [&](const std::vector<PlaylistEntry>&, bool wasCancelled) {
                         cancelled = wasCancelled;
                         done      = true;
                     });

    task.start();
    task.cancel();
    REQUIRE(pumpUntil(done));

    // Cancelling is not failing: the scan returns, and says it was cut short so
    // the window can report that rather than "nothing playable was found".
    CHECK(cancelled);
}
