// The hop onto the interface thread, and getting an answer back off it.
//
// This is the part of the remote control that is genuinely hard, and every case
// here is a failure that was reasoned about rather than observed -- which is
// exactly why they are pinned. Nothing sleeps waiting for a timer: the
// dispatcher is a queue the test drains where it chooses, so a hang here is a
// bug rather than a slow machine.

#include "../FakePlayerControl.hpp"

#include "CallGate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

using namespace xpcog;
using namespace xpcog::remote;
using xpcog::test::FakePlayerControl;

namespace {

/// A dispatcher that holds what it is given until the test says otherwise.
///
/// The point of the whole suite: nothing here waits for wall-clock time to make
/// the interesting thing happen, so the interesting thing happens exactly where
/// the test puts it.
class QueueDispatcher {
public:
    [[nodiscard]] Dispatcher dispatcher() {
        return [this](std::function<void()> job) {
            std::lock_guard guard(mutex_);
            queued_.push_back(std::move(job));
        };
    }

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard guard(mutex_);
        return queued_.size();
    }

    /// Runs everything queued so far, on the calling thread.
    void drain() {
        std::deque<std::function<void()>> ready;
        {
            std::lock_guard guard(mutex_);
            ready.swap(queued_);
        }
        for (std::function<void()>& job : ready) {
            job();
        }
    }

private:
    mutable std::mutex                mutex_;
    std::deque<std::function<void()>> queued_;
};

constexpr std::chrono::milliseconds kPatience{2000};
constexpr std::chrono::milliseconds kImpatience{50};

}  // namespace

TEST_CASE("an inline dispatcher does not deadlock", "[remote]") {
    // The configuration that would hang if the gate took its lock before
    // dispatching: the closure runs on this thread, inside call(), and would try
    // to take a mutex this thread already holds. xpcog-cli's dispatcher does
    // this whenever it is already on its executor, so it is not a contrived case.
    FakePlayerControl control;
    CallGate          gate(control, [](std::function<void()> job) { job(); }, kPatience);

    const auto answer = gate.call([](IPlayerControl& player) { return player.status(); });

    REQUIRE(answer.has_value());
    CHECK(control.countOf("status") == 1);
}

TEST_CASE("nothing happens until the interface thread runs it", "[remote]") {
    FakePlayerControl control;
    QueueDispatcher   dispatcher;
    CallGate          gate(control, dispatcher.dispatcher(), kPatience);

    std::atomic<bool> finished{false};
    std::thread       caller([&] {
        const auto answer =
            gate.call([](IPlayerControl& player) { return player.playPause(); });
        CHECK(answer.has_value());
        finished.store(true);
    });

    // The work is queued and the caller is waiting on it. Spinning on `pending`
    // rather than sleeping: the queue is what the test is about.
    while (dispatcher.pending() == 0) {
        std::this_thread::yield();
    }
    CHECK_FALSE(finished.load());
    CHECK(control.countOf("playPause") == 0);

    dispatcher.drain();
    caller.join();

    CHECK(finished.load());
    CHECK(control.countOf("playPause") == 1);
}

TEST_CASE("an interface that never answers times out rather than hanging", "[remote]") {
    FakePlayerControl control;
    QueueDispatcher   dispatcher;
    CallGate          gate(control, dispatcher.dispatcher(), kImpatience);

    // Queued and never drained, which is what a wedged interface thread looks
    // like from here.
    const auto answer = gate.call([](IPlayerControl& player) { return player.stop(); });

    CHECK_FALSE(answer.has_value());
    CHECK(control.countOf("stop") == 0);
}

TEST_CASE("draining after the caller gave up touches nothing freed", "[remote]") {
    // The reason the slot is a shared_ptr captured by value rather than a
    // promise on the caller's stack. A slow interface thread runs the closure
    // after the wait has timed out and, in the application, after the frame that
    // owned the promise has gone. Under ASan this is the case that would say so.
    FakePlayerControl control;
    QueueDispatcher   dispatcher;

    {
        CallGate gate(control, dispatcher.dispatcher(), kImpatience);
        const auto answer =
            gate.call([](IPlayerControl& player) { return player.next(); });
        CHECK_FALSE(answer.has_value());
        // The gate is still alive here, which is the contract: RemoteServer::stop()
        // joins the listener before anything is destroyed.
        dispatcher.drain();
        CHECK(control.countOf("next") == 1);
    }
}

TEST_CASE("closing releases a waiter at once", "[remote]") {
    FakePlayerControl control;
    QueueDispatcher   dispatcher;
    // A patience long enough that a test which actually waited it out would be
    // obvious. close() is what makes this return promptly.
    CallGate gate(control, dispatcher.dispatcher(), std::chrono::milliseconds{60000});

    std::atomic<bool> finished{false};
    std::thread       caller([&] {
        const auto answer =
            gate.call([](IPlayerControl& player) { return player.previous(); });
        CHECK_FALSE(answer.has_value());
        finished.store(true);
    });

    while (dispatcher.pending() == 0) {
        std::this_thread::yield();
    }
    gate.close();
    caller.join();

    CHECK(finished.load());
    CHECK(gate.closed());
}

TEST_CASE("a closed gate refuses without dispatching", "[remote]") {
    FakePlayerControl control;
    QueueDispatcher   dispatcher;
    CallGate          gate(control, dispatcher.dispatcher(), kPatience);

    gate.close();
    const auto answer = gate.call([](IPlayerControl& player) { return player.status(); });

    CHECK_FALSE(answer.has_value());
    // Not merely unanswered -- never queued. Once the server is stopping there
    // is nothing to be gained by handing the interface thread more work.
    CHECK(dispatcher.pending() == 0);
}

TEST_CASE("the answer comes back, not just the fact of one", "[remote]") {
    FakePlayerControl control;
    control.statusValue.playing      = true;
    control.statusValue.position     = 91.5;
    control.statusValue.playlistSize = 312;

    CallGate gate(control, [](std::function<void()> job) { job(); }, kPatience);

    const auto answer = gate.call([](IPlayerControl& player) { return player.status(); });

    REQUIRE(answer.has_value());
    CHECK(answer->playing);
    CHECK(answer->position == 91.5);
    CHECK(answer->playlistSize == 312);
}

TEST_CASE("a declined command comes back as declined", "[remote]") {
    // PlaybackController silently declines while a start is in flight. The gate
    // must carry that through rather than flattening it into "it happened".
    FakePlayerControl control;
    control.outcome = Outcome::Busy;

    CallGate gate(control, [](std::function<void()> job) { job(); }, kPatience);

    const auto answer = gate.call([](IPlayerControl& player) { return player.play({}); });

    REQUIRE(answer.has_value());
    CHECK(*answer == Outcome::Busy);
}
