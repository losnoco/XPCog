// The one-thread executor behind PlaybackController's blocking calls.
//
// Three properties, all of which the QThread it replaced provided for free and
// none of which is obvious in a hand-rolled version: tasks run in order, they run
// somewhere else, and destruction does not wait for the queue.

#include "xpcog/core/SerialExecutor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace xpcog;

namespace {

/// Blocks until counted to `target`, or gives up -- so a failure is an assertion
/// rather than a test run that never finishes.
template <typename Predicate>
bool waitFor(Predicate ready, int milliseconds = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

}  // namespace

TEST_CASE("tasks run in the order they were posted", "[core][executor]") {
    std::mutex               mutex;
    std::vector<int>         ran;
    std::atomic<int>         count{0};

    {
        SerialExecutor executor;
        for (int i = 0; i < 50; ++i) {
            executor.post([&, i] {
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    ran.push_back(i);
                }
                ++count;
            });
        }
        REQUIRE(waitFor([&] { return count.load() == 50; }));
    }

    std::vector<int> expected(50);
    for (int i = 0; i < 50; ++i) {
        expected[static_cast<std::size_t>(i)] = i;
    }
    CHECK(ran == expected);
}

TEST_CASE("tasks run off the posting thread", "[core][executor]") {
    std::atomic<bool> done{false};
    std::thread::id   ranOn;

    SerialExecutor executor;
    executor.post([&] {
        ranOn = std::this_thread::get_id();
        done  = true;
    });

    REQUIRE(waitFor([&] { return done.load(); }));

    // The whole reason this class exists: play() and stop() block, and blocking
    // on the interface's thread is a frozen window.
    CHECK(ranOn != std::this_thread::get_id());
}

TEST_CASE("only one task runs at a time", "[core][executor]") {
    std::atomic<int> concurrent{0};
    std::atomic<int> peak{0};
    std::atomic<int> finished{0};

    SerialExecutor executor;
    for (int i = 0; i < 20; ++i) {
        executor.post([&] {
            const int now = ++concurrent;
            int       seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --concurrent;
            ++finished;
        });
    }

    REQUIRE(waitFor([&] { return finished.load() == 20; }));

    // Serial is the contract, not an implementation detail: two starts running
    // together would be two threads inside the engine reconfiguring one device.
    CHECK(peak.load() == 1);
}

TEST_CASE("destruction does not wait for the queue to drain", "[core][executor]") {
    std::atomic<int>        ran{0};
    std::mutex              mutex;
    std::condition_variable released;
    bool                    go = false;

    {
        SerialExecutor executor;

        // The first task blocks until released, so everything behind it is still
        // queued when the executor goes away.
        executor.post([&] {
            std::unique_lock<std::mutex> lock(mutex);
            released.wait(lock, [&] { return go; });
            ++ran;
        });
        for (int i = 0; i < 100; ++i) {
            executor.post([&] { ++ran; });
        }

        {
            const std::lock_guard<std::mutex> lock(mutex);
            go = true;
        }
        released.notify_all();
        // Leaving the scope here joins. The task already running finishes; the
        // hundred behind it are dropped rather than run, which is what stops a
        // window closing from taking as long as whatever was queued.
    }

    CHECK(ran.load() < 101);
}
