// Getting an answer out of the interface thread, from a thread that is not it.
//
// Almost nothing in this program may be touched from an HTTP worker. Playlist,
// PlaylistView, UndoStack, Library, Settings and Signal are all unlocked and
// single-threaded by convention; PluginCache is explicitly unsynchronised. The
// established answer is platform::MediaIntegration's -- take a Dispatcher and
// hop -- but that one is fire-and-forget, and an HTTP request has to come back
// with something. This is the difference.
//
// --- Why the slot is a shared_ptr ------------------------------------------
//
// The obvious shape is a std::promise on the caller's stack and a future to wait
// on. It is wrong here, and the way it is wrong is a use-after-free with a delay
// on it.
//
// Two things can outlive the waiter. wxEvtHandler::CallAfter drops pending
// events when the handler is destroyed, so a closure may never run at all; and
// an interface thread that is merely slow may run it after the wait has timed
// out and the frame has gone. Either way the closure must be safe to run into
// nothing. So the shared state is heap-allocated and captured *by value*: if the
// waiter has left, the closure writes into a block nobody is reading and drops
// the last reference on the way out.
//
// --- Why dispatch happens before the lock ----------------------------------
//
// Some dispatchers run their callable inline -- xpcog-cli's does when it is
// already on its executor, and every test's does. Taking the mutex first and
// then dispatching would have the closure try to take it again on the same
// thread, which is a deadlock in the one configuration that is easiest to test
// in. Dispatching first means an inline dispatcher has already finished by the
// time the predicate is checked, and the wait returns immediately.
//
// --- The timeout is 503, and it is not a lie ------------------------------
//
// Two seconds, then the request answers 503 with Retry-After. Not 504: nothing
// here is a gateway, the interface is busy. A modal dialog does *not* cause this
// -- wx modals pump a nested event loop, so CallAfter still runs -- and the real
// causes are a long synchronous handler and shutdown. Answering 200 for a call
// that never happened would be the first place this API lied.

#pragma once

#include "xpcog/core/Dispatcher.hpp"
#include "xpcog/core/remote/PlayerControl.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace xpcog::remote {

class CallGate {
public:
    CallGate(IPlayerControl& control, Dispatcher dispatch,
             std::chrono::milliseconds timeout)
        : control_(control), dispatch_(std::move(dispatch)), timeout_(timeout) {}

    CallGate(const CallGate&)            = delete;
    CallGate& operator=(const CallGate&) = delete;

    /// Runs `job` on the interface thread and waits for what it returns.
    ///
    /// nullopt means the interface did not answer in time, or the gate has been
    /// closed because the server is stopping. The caller turns that into 503;
    /// there is deliberately no way to tell those two apart from here, because
    /// the answer to both is the same and the difference is not the client's.
    template <typename F>
    [[nodiscard]] auto call(F job)
        -> std::optional<std::invoke_result_t<F, IPlayerControl&>> {
        using Result = std::invoke_result_t<F, IPlayerControl&>;

        if (closed_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        struct Slot {
            bool                  done = false;
            std::optional<Result> value;
        };
        auto slot = std::make_shared<Slot>();

        // Captured by value, and `this` alongside it only for the mutex and the
        // condition variable -- both of which outlive the gate's users by the
        // stop() contract in RemoteServer.hpp.
        dispatch_([this, slot, job = std::move(job)]() mutable {
            Result value = job(control_);
            {
                std::lock_guard guard(mutex_);
                slot->value = std::move(value);
                slot->done  = true;
            }
            done_.notify_all();
        });

        std::unique_lock lock(mutex_);
        done_.wait_for(lock, timeout_, [&] {
            return slot->done || closed_.load(std::memory_order_relaxed);
        });

        if (!slot->done) {
            return std::nullopt;
        }
        return std::move(slot->value);
    }

    /// Releases every waiter at once.
    ///
    /// Called first in RemoteServer::stop(), so that quitting does not make each
    /// request in flight sit out its full timeout before the listener can join.
    void close() {
        {
            std::lock_guard guard(mutex_);
            closed_.store(true, std::memory_order_release);
        }
        done_.notify_all();
    }

    [[nodiscard]] bool closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

private:
    IPlayerControl&           control_;
    Dispatcher                dispatch_;
    std::chrono::milliseconds timeout_;

    // One mutex for the whole gate rather than one per call. The contention is a
    // handful of concurrent requests, not a hot path, and a mutex per call would
    // be a mutex to allocate and destroy on every request.
    std::mutex              mutex_;
    std::condition_variable done_;
    std::atomic<bool>       closed_{false};
};

}  // namespace xpcog::remote
