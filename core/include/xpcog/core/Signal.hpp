// Observation without Qt or KVO.
//
// Cog uses KVO and NSNotificationCenter, neither of which exists here and both of
// which are unowned: an observer that outlives its subject crashes, and one that
// forgets to deregister leaks. `Subscription` is an RAII token instead -- holding
// it is what keeps the connection alive, and dropping it disconnects.
//
// Deliberately small. This is not a signal/slot framework; the app layer has Qt's
// for that. It exists so core can notify without depending on one.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace xpcog {

namespace detail {
struct SignalBase {
    virtual ~SignalBase()                       = default;
    virtual void disconnect(std::uint64_t id) noexcept = 0;
};
}  // namespace detail

/// Move-only handle to one connection. Disconnects on destruction.
class Subscription {
public:
    Subscription() = default;
    Subscription(std::weak_ptr<detail::SignalBase> owner, std::uint64_t id)
        : owner_(std::move(owner)), id_(id) {}

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : owner_(std::move(other.owner_)), id_(std::exchange(other.id_, 0)) {}

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::move(other.owner_);
            id_    = std::exchange(other.id_, 0);
        }
        return *this;
    }

    ~Subscription() { reset(); }

    /// Disconnects early. Safe if the signal is already gone.
    void reset() noexcept {
        if (id_ != 0) {
            if (const auto owner = owner_.lock()) {
                owner->disconnect(id_);
            }
            id_ = 0;
        }
        owner_.reset();
    }

    /// Keeps the connection for as long as the program runs. Only for observers
    /// that genuinely live forever; anything else should hold the token.
    void release() noexcept {
        id_ = 0;
        owner_.reset();
    }

    [[nodiscard]] bool connected() const noexcept { return id_ != 0 && !owner_.expired(); }

private:
    std::weak_ptr<detail::SignalBase> owner_;
    std::uint64_t                     id_ = 0;
};

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(const Args&...)>;

    Signal() : impl_(std::make_shared<Impl>()) {}

    Signal(const Signal&)            = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) noexcept        = default;
    Signal& operator=(Signal&&) noexcept = default;

    [[nodiscard]] Subscription connect(Slot slot) {
        const std::uint64_t id = impl_->nextId++;
        impl_->slots.emplace_back(id, std::move(slot));
        return Subscription{impl_, id};
    }

    /// Slots may connect or disconnect from inside a slot, so the list is copied
    /// before dispatch. Emission is not reentrant-safe beyond that -- a slot that
    /// emits the same signal recursively is a caller bug, not something to guard.
    void emit(const Args&... args) const {
        const auto slots = impl_->slots;
        for (const auto& [id, slot] : slots) {
            if (slot) {
                slot(args...);
            }
        }
    }

    [[nodiscard]] bool empty() const noexcept { return impl_->slots.empty(); }

private:
    struct Impl final : detail::SignalBase {
        std::vector<std::pair<std::uint64_t, Slot>> slots;
        std::uint64_t                               nextId = 1;

        void disconnect(std::uint64_t id) noexcept override {
            for (auto it = slots.begin(); it != slots.end(); ++it) {
                if (it->first == id) {
                    slots.erase(it);
                    return;
                }
            }
        }
    };

    std::shared_ptr<Impl> impl_;
};

}  // namespace xpcog
