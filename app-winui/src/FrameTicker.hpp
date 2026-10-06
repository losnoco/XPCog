#pragma once

// What paces the painted visualisers: XAML's per-frame Rendering event, thinned
// to the pane's own rate. GTK's views run off a GLib timeout at the same
// rates, which has no such trouble.
//
// Not a DispatcherQueueTimer, which is what this replaced. Its ticks run below
// idle priority, and XAML queues its own frames at idle priority as well, so
// with nothing else waking the thread both came late: the scope and the
// analyser stuttered until the pointer moved over the window, and smoothed out
// while it did (microsoft-ui-xaml#11144). A Rendering handler keeps XAML
// producing a frame per vertical blank for as long as it is attached, which is
// why this one is attached only while a pane is shown and something plays.

#include "WinRT.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace xpcog::winui {

class FrameTicker {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameTicker(std::function<void()> onTick) : onTick_(std::move(onTick)) {}

    ~FrameTicker() {
        // After the XAML loop has ended a call into it throws, and a throw out
        // of a destructor is std::terminate; with the loop gone there are no
        // more frames to stop anyway.
        try {
            stop();
        } catch (const winrt::hresult_error&) {
        }
    }

    FrameTicker(const FrameTicker&)            = delete;
    FrameTicker& operator=(const FrameTicker&) = delete;

    /// The shortest gap between ticks. A frame that comes sooner is skipped,
    /// so the rate is this or the display's, whichever is lower.
    void setInterval(Clock::duration interval) { interval_ = interval; }

    void start() {
        if (token_) {
            return;
        }
        due_   = Clock::now();
        token_ = mux::Media::CompositionTarget::Rendering(
            [this](auto&&, auto&&) { frame(); });
    }

    void stop() {
        if (!token_) {
            return;
        }
        const winrt::event_token token = std::exchange(token_, {});
        mux::Media::CompositionTarget::Rendering(token);
    }

private:
    void frame() {
        const Clock::time_point now = Clock::now();
        // Half a millisecond of slack, so a 60 Hz pane on a 60 Hz display does
        // not skip the frames that land a hair early.
        if (now + std::chrono::microseconds(500) < due_) {
            return;
        }
        // Step the deadline rather than restart it from now, so a rate that
        // does not divide the display's averages out to itself; after a stall
        // it starts over instead of ticking to catch up.
        due_ += interval_;
        if (due_ < now) {
            due_ = now + interval_;
        }
        onTick_();
    }

    std::function<void()> onTick_;
    Clock::duration       interval_ = std::chrono::milliseconds(16);
    Clock::time_point     due_{};
    winrt::event_token    token_{};
};

}  // namespace xpcog::winui
