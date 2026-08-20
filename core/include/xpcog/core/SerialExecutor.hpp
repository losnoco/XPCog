// One thread, a queue, and nothing else.
//
// Replaces the QThread-plus-bare-QObject pair PlaybackController used to own,
// whose only purpose was to have an event loop something could be queued onto.
// Without Qt that idiom has no spelling, and it was always more machinery than
// the job needed: what is wanted is "run this off the calling thread, in order,
// one at a time".
//
// The job is AudioEngine::play() and stop(), which block. Starting a track opens
// its source and primes about a second and a half of audio -- microseconds for a
// file, a network round trip for a URL, and a station slow to answer froze the
// window for as long as it took. Cog opens URLs from a background queue
// (-addURLsInBackground:) for the same reason.
//
// Serial rather than a pool, and that is the contract rather than an
// implementation detail: two starts running concurrently would be two threads
// inside the engine reconfiguring the same device.
//
// Nothing here marshals results back. A task that has an answer hands it over
// itself -- through the caller's own dispatcher -- because this class has no idea
// what thread the answer belongs on.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xpcog {

class SerialExecutor {
public:
    SerialExecutor();

    SerialExecutor(const SerialExecutor&)            = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    /// Stops accepting work, lets the task already running finish, and joins.
    ///
    /// Tasks still queued are dropped rather than run. A destructor that drained
    /// the queue would make destruction take as long as whatever was waiting,
    /// which for this executor's actual job means a window that will not close
    /// while a slow URL is being opened.
    ~SerialExecutor();

    /// Queues `task`. Returns at once. Does nothing once the executor is
    /// stopping, so a post racing with destruction is dropped rather than run on
    /// a half-destroyed owner.
    void post(std::function<void()> task);

private:
    void run();

    std::mutex                        mutex_;
    std::condition_variable           wake_;
    std::deque<std::function<void()>> queue_;
    bool                              stopping_ = false;

    /// Last, so the thread cannot start before the state it reads is built.
    std::thread thread_;
};

}  // namespace xpcog
