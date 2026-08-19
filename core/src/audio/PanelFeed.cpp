#include "xpcog/core/audio/PanelFeed.hpp"

#include <algorithm>

namespace xpcog {

PanelFeed& PanelFeed::instance() {
    // Function-local, so construction is ordered and there is no static
    // initialisation order to reason about. Never destroyed: a decode thread
    // may still be posting while the process tears down, and a queue that has
    // been destructed underneath it is worse than one that leaks at exit.
    static PanelFeed* feed = new PanelFeed();
    return *feed;
}

bool PanelFeed::producing() const noexcept {
    return produced_.load(std::memory_order_relaxed);
}

bool PanelFeed::wanted() const noexcept {
    return wanted_.load(std::memory_order_relaxed);
}

void PanelFeed::setWanted(bool wanted) noexcept {
    wanted_.store(wanted, std::memory_order_relaxed);
    if (!wanted) {
        // Drop the queued states, which describe a panel nobody is looking at
        // -- but keep knowing which track is audible. Forgetting that as well
        // meant a display closed and reopened during one track stayed empty
        // until the *next* track began, because nothing else ever says which
        // one is playing.
        flush();
        produced_.store(false, std::memory_order_relaxed);
    }
}

void PanelFeed::post(const Url& track, double seconds,
                     std::span<const std::byte> state) {
    std::lock_guard lock(mutex_);

    auto it = std::find_if(tracks_.begin(), tracks_.end(),
                           [&track](const Track& entry) { return entry.url == track; });
    if (it == tracks_.end()) {
        // Oldest first, so the front is the one that has been waiting longest --
        // which after a gapless seam is the one that just finished.
        while (tracks_.size() >= kMaxTracks) {
            tracks_.pop_front();
        }
        tracks_.push_back(Track{track, {}});
        it = std::prev(tracks_.end());
    }

    produced_.store(true, std::memory_order_relaxed);

    PanelFrame frame;
    frame.seconds = seconds;
    frame.state.assign(state.begin(), state.end());
    it->frames.push_back(std::move(frame));

    // Trimmed here rather than on a timer: a panel nobody is draining is
    // exactly the case this bounds, and a paused player drains nothing.
    const double oldest = seconds - kMaxHistorySeconds;
    while (!it->frames.empty() && it->frames.front().seconds < oldest) {
        it->frames.pop_front();
    }
}

void PanelFeed::setAudibleTrack(const Url& track) {
    std::lock_guard lock(mutex_);
    audible_     = track;
    haveAudible_ = true;

    // Anything queued before the one now audible describes a track that has
    // finished playing, so it will never be drained.
    while (!tracks_.empty() && !(tracks_.front().url == track)) {
        tracks_.pop_front();
    }
}

std::vector<PanelFrame> PanelFeed::take(double seconds) {
    std::lock_guard lock(mutex_);
    std::vector<PanelFrame> out;
    if (!haveAudible_) {
        return out;
    }

    auto it = std::find_if(tracks_.begin(), tracks_.end(), [this](const Track& entry) {
        return entry.url == audible_;
    });
    if (it == tracks_.end()) {
        return out;
    }

    while (!it->frames.empty() && it->frames.front().seconds <= seconds) {
        out.push_back(std::move(it->frames.front()));
        it->frames.pop_front();
    }
    return out;
}

std::optional<PanelFrame> PanelFeed::peekEarliest() const {
    std::lock_guard lock(mutex_);
    if (!haveAudible_) {
        return std::nullopt;
    }
    const auto it = std::find_if(tracks_.begin(), tracks_.end(), [this](const Track& entry) {
        return entry.url == audible_;
    });
    if (it == tracks_.end() || it->frames.empty()) {
        return std::nullopt;
    }
    return it->frames.front();
}

void PanelFeed::flush() {
    std::lock_guard lock(mutex_);
    for (Track& track : tracks_) {
        track.frames.clear();
    }
}

void PanelFeed::forget(const Url& track) {
    std::lock_guard lock(mutex_);
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&track](const Track& entry) {
                                     return entry.url == track;
                                 }),
                  tracks_.end());
}

void PanelFeed::clear() {
    std::lock_guard lock(mutex_);
    tracks_.clear();
    audible_     = Url{};
    haveAudible_ = false;
    produced_.store(false, std::memory_order_relaxed);
}

}  // namespace xpcog
