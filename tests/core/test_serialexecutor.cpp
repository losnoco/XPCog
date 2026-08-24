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
    // Each queued task is slow enough that draining all of them would take about
    // a second, and destruction has to return in a small fraction of that. The
    // margin is the point: the difference between "dropped the queue" and "ran
    // the queue" is three orders of magnitude here rather than a scheduling
    // accident.
    //
    // **This used to assert the outcome of a race**, and is worth keeping the
    // history of. It queued a hundred `++ran` calls behind a blocking one,
    // released the blocker, and checked that fewer than all of them had run --
    // which held only if the destructor set `stopping_` before the worker, newly
    // woken from a condition variable, could get through a hundred trivial
    // tasks. The main thread usually won that race, so it usually passed. macOS
    // on arm64 was fast enough to lose it, and the resulting red build was a
    // property of the runner rather than of the code.
    //
    // The claim being made is the one in the test's name, so time is what it
    // ought to measure.
    constexpr int  kQueued  = 100;
    constexpr auto kPerTask = std::chrono::milliseconds(10);

    std::atomic<int>  ran{0};
    std::atomic<bool> started{false};

    const auto begin = std::chrono::steady_clock::now();
    {
        SerialExecutor executor;

        // Signals that the worker is genuinely running, so the scope below is
        // left with the queue occupied rather than possibly not yet picked up.
        executor.post([&] {
            ++ran;
            started.store(true);
        });
        for (int i = 0; i < kQueued; ++i) {
            executor.post([&] {
                std::this_thread::sleep_for(kPerTask);
                ++ran;
            });
        }

        REQUIRE(waitFor([&] { return started.load(); }));
        // Leaving the scope drops what is still queued and joins, waiting only
        // for the one task in flight -- which is what stops a window closing
        // from taking as long as whatever was queued behind it.
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    // The queue was dropped rather than run.
    CHECK(ran.load() < kQueued + 1);

    // And destruction took nothing like as long as draining would have. Half the
    // total is deliberately loose: the true figure is one task, and a bound this
    // wide cannot go red merely for running on a busy machine.
    CHECK(elapsed < (kQueued * kPerTask) / 2);
}
