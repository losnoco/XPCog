#include "xpcog/core/audio/WaveformProvider.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/PluginRegistry.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace xpcog {

struct WaveformProvider::Impl {
    Impl(const PluginRegistry& registry_, WaveformCache cache_, Dispatcher dispatch_)
        : registry(registry_), cache(std::move(cache_)), dispatch(std::move(dispatch_)) {}

    const PluginRegistry& registry;
    const WaveformCache   cache;
    const Dispatcher      dispatch;

    Signal<Url, std::shared_ptr<const WaveformSummary>> updated;

    std::atomic<std::uint64_t> generation{0};

    std::mutex         mutex;
    std::optional<Url> pendingPrefetch;  // guarded by mutex
    bool               requestDone = false;  // guarded by mutex; the foreground job of this generation has finished
    IDecoder*          active      = nullptr;  // guarded by mutex; the decoder a job is reading, for interrupt()

    /// Bumps the generation and pokes whatever is reading, so a job blocked in
    /// a network read lets go rather than waiting for the next chunk to notice.
    std::uint64_t supersede() {
        const std::uint64_t next = ++generation;
        const std::lock_guard lock(mutex);
        pendingPrefetch.reset();
        requestDone = false;
        if (active != nullptr) {
            active->interrupt();
        }
        return next;
    }

    void publish(std::uint64_t forGeneration, const Url& url,
                 std::shared_ptr<const WaveformSummary> summary, std::weak_ptr<Impl> self) {
        dispatch([self = std::move(self), forGeneration, url, summary = std::move(summary)] {
            const std::shared_ptr<Impl> impl = self.lock();
            if (!impl || impl->generation.load() != forGeneration) {
                return;
            }
            impl->updated.publish(url, summary);
        });
    }

    /// The whole of one job. Runs on the executor's thread.
    void run(std::uint64_t forGeneration, const Url& url, bool foreground,
             std::weak_ptr<Impl> self, SerialExecutor& executor) {
        const auto superseded = [this, forGeneration] {
            return generation.load() != forGeneration;
        };
        if (superseded()) {
            return;
        }

        if (std::optional<WaveformSummary> hit = cache.load(url)) {
            publish(forGeneration, url,
                    std::make_shared<const WaveformSummary>(std::move(*hit)), self);
        } else if (auto opened = registry.open(url, SkipCue::No, LoopPolicy::Never)) {
            {
                const std::lock_guard lock(mutex);
                active = opened.decoder.get();
            }
            WaveformSummary summary;
            const bool      done = analyseWaveform(
                *opened.decoder, summary, superseded,
                [&](const WaveformSummary& partial) {
                    publish(forGeneration, url, std::make_shared<const WaveformSummary>(partial),
                            self);
                });
            {
                const std::lock_guard lock(mutex);
                active = nullptr;
            }
            if (done) {
                cache.store(url, summary);
                publish(forGeneration, url,
                        std::make_shared<const WaveformSummary>(std::move(summary)), self);
            }
        }

        if (!foreground || superseded()) {
            return;
        }

        // The foreground is done with; the guess, if one has been made, may go.
        std::optional<Url> next;
        {
            const std::lock_guard lock(mutex);
            requestDone = true;
            next        = std::exchange(pendingPrefetch, std::nullopt);
        }
        if (next) {
            executor.post([self, forGeneration, url = *next, &executor] {
                if (const std::shared_ptr<Impl> impl = self.lock()) {
                    impl->run(forGeneration, url, /*foreground=*/false, self, executor);
                }
            });
        }
    }
};

WaveformProvider::WaveformProvider(const PluginRegistry& registry, WaveformCache cache,
                                   Dispatcher dispatch)
    : impl_(std::make_shared<Impl>(registry, std::move(cache), std::move(dispatch))) {}

WaveformProvider::~WaveformProvider() {
    impl_->supersede();
    // executor_ joins next, in member order; the running job sees the bump
    // within one read and returns.
}

void WaveformProvider::request(const Url& url) {
    const std::uint64_t generation = impl_->supersede();
    executor_.post([self = std::weak_ptr<Impl>(impl_), generation, url, this] {
        // Strong for the job's duration: the destructor joins this before it
        // lets impl_ go, so the lock always succeeds, but the job holds its own
        // reference all the same rather than lean on that ordering.
        if (const std::shared_ptr<Impl> impl = self.lock()) {
            impl->run(generation, url, /*foreground=*/true, self, executor_);
        }
    });
}

void WaveformProvider::prefetch(const Url& url) {
    const std::uint64_t generation = impl_->generation.load();
    bool                startNow   = false;
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->requestDone) {
            startNow = true;
        } else {
            impl_->pendingPrefetch = url;
        }
    }
    if (startNow) {
        executor_.post([self = std::weak_ptr<Impl>(impl_), generation, url, this] {
            if (const std::shared_ptr<Impl> impl = self.lock()) {
                impl->run(generation, url, /*foreground=*/false, self, executor_);
            }
        });
    }
}

void WaveformProvider::cancel() {
    impl_->supersede();
}

Signal<Url, std::shared_ptr<const WaveformSummary>>& WaveformProvider::updated() noexcept {
    return impl_->updated;
}

}  // namespace xpcog
