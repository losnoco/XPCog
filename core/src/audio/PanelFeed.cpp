#include "xpcog/core/audio/PanelFeed.hpp"

#include <algorithm>
#include <type_traits>

namespace xpcog {

PanelFeed& PanelFeed::instance() {
    // Function-local, so construction is ordered and there is no static
    // initialisation order to reason about. Never destroyed: a decode thread
    // may still be posting while the process tears down, and a queue that has
    // been destructed underneath it is worse than one that leaks at exit.
    static PanelFeed* feed = new PanelFeed();
    return *feed;
}

namespace {

/// The audible track's entry, or null.
template <typename Tracks, typename Url>
auto* findAudible(Tracks& tracks, const Url& audible, bool have) {
    using Entry = std::remove_reference_t<decltype(*tracks.begin())>;
    if (!have) {
        return static_cast<Entry*>(nullptr);
    }
    for (Entry& entry : tracks) {
        if (entry.url == audible) {
            return &entry;
        }
    }
    return static_cast<Entry*>(nullptr);
}

}  // namespace

bool PanelFeed::producing() const {
    std::lock_guard lock(mutex_);
    const auto* entry = findAudible(tracks_, audible_, haveAudible_);
    return entry != nullptr && !entry->frames.empty();
}

std::optional<PanelFrame> PanelFeed::stateAt(double seconds) {
    std::lock_guard lock(mutex_);
    auto* entry = findAudible(tracks_, audible_, haveAudible_);
    if (entry == nullptr || entry->frames.empty()) {
        return std::nullopt;
    }

    // The newest state at or before `seconds`. Walking from the front is right
    // rather than merely convenient: a display asks roughly thirty times a
    // second and everything before its answer is dropped, so each state is
    // stepped over once in the life of a track.
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < entry->frames.size(); ++i) {
        if (entry->frames[i].seconds > seconds) {
            break;
        }
        chosen = i;
    }

    PanelFrame frame = entry->frames[chosen];
    // Everything before the answer is unreachable now: positions only advance,
    // and the one thing that could go backwards -- a seek -- throws the whole
    // track's history away anyway.
    for (std::size_t i = 0; i < chosen; ++i) {
        entry->frames.pop_front();
    }
    return frame;
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

    PanelFrame frame;
    frame.seconds = seconds;
    frame.state.assign(state.begin(), state.end());
    it->frames.push_back(std::move(frame));

    // Trimmed here rather than on a timer: what is bounded is the history of a
    // track nobody is looking at, and a timer is the one thing that could fall
    // behind a decoder running far ahead of real time.
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
}

}  // namespace xpcog
