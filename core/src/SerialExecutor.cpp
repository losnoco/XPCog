#include "xpcog/core/SerialExecutor.hpp"

#include <utility>

namespace xpcog {

SerialExecutor::SerialExecutor() : thread_([this] { run(); }) {}

SerialExecutor::~SerialExecutor() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // Dropped rather than drained; see the header for why waiting here would
        // be the wrong trade.
        queue_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SerialExecutor::post(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void SerialExecutor::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // Outside the lock, which is the whole point: the task blocks, and
        // holding the mutex across it would make post() block with it.
        task();
    }
}

}  // namespace xpcog
