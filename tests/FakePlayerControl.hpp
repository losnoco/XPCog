// A scripted IPlayerControl, in FakeHttp.hpp's style.
//
// Hand-written rather than generated: there is no gMock in this tree, and the
// interesting part of a double here is not "was this called" but "what does the
// router do with the answer". So every method has a value the test can set and a
// record of what it was passed, and the awkward cases -- Busy, NotFound,
// Unsupported -- are set the same way as the ordinary ones.
//
// Thread-safe, because the gate calls it from whichever thread the test's
// dispatcher runs on, and some of those tests drain from a second one.

#pragma once

#include "xpcog/core/remote/PlayerControl.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace xpcog::test {

class FakePlayerControl : public remote::IPlayerControl {
public:
    // --- what it will answer ------------------------------------------------

    remote::Status                            statusValue;
    remote::Outcome                           outcome = remote::Outcome::Ok;
    std::vector<remote::TrackSummary>         trackList;
    std::optional<remote::TrackDetail>        trackDetail;
    std::vector<remote::SettingInfo>          settingList;
    std::optional<remote::SettingInfo>        settingValue;
    remote::SettingWrite                      settingWrite;
    remote::EqualizerState                    equalizerState;
    std::vector<std::string>                  presetList;
    std::string                               jobId = "job-1";
    std::optional<remote::JobStatus>          jobValue;
    std::shared_ptr<const std::vector<std::byte>> artworkValue;

    // --- what it was asked --------------------------------------------------

    struct Call {
        std::string          name;
        std::vector<TrackId> ids;
        double               number = 0.0;
        std::string          text;
    };

    [[nodiscard]] std::vector<Call> calls() const {
        std::lock_guard guard(mutex_);
        return calls_;
    }

    [[nodiscard]] std::size_t countOf(std::string_view name) const {
        std::lock_guard guard(mutex_);
        std::size_t     count = 0;
        for (const Call& call : calls_) {
            if (call.name == name) {
                ++count;
            }
        }
        return count;
    }

    // --- IPlayerControl -----------------------------------------------------

    remote::Status status() override {
        note(Call{"status"});
        return statusValue;
    }

    remote::Outcome play(std::optional<TrackId> id) override {
        Call call{"play", {}, 0.0, {}};
        if (id) {
            call.ids.push_back(*id);
        }
        note(std::move(call));
        return outcome;
    }
    remote::Outcome pause() override { note(Call{"pause"}); return outcome; }
    remote::Outcome playPause() override { note(Call{"playPause"}); return outcome; }
    remote::Outcome stop() override { note(Call{"stop"}); return outcome; }
    remote::Outcome next() override { note(Call{"next"}); return outcome; }
    remote::Outcome previous() override { note(Call{"previous"}); return outcome; }

    remote::Outcome seek(double seconds) override {
        note(Call{"seek", {}, seconds, {}});
        return outcome;
    }
    remote::Outcome setVolume(double linear) override {
        note(Call{"setVolume", {}, linear, {}});
        return outcome;
    }
    remote::Outcome setOrder(std::optional<std::string> repeat,
                             std::optional<std::string> shuffle,
                             std::optional<bool>        stopAfterCurrent) override {
        std::string text = repeat.value_or("-") + "/" + shuffle.value_or("-") + "/" +
                           (stopAfterCurrent ? (*stopAfterCurrent ? "1" : "0") : "-");
        note(Call{"setOrder", {}, 0.0, std::move(text)});
        return outcome;
    }

    std::vector<remote::TrackSummary> tracks(std::size_t offset, std::size_t limit,
                                             std::string_view query,
                                             std::size_t&     total) override {
        note(Call{"tracks", {}, static_cast<double>(offset), std::string{query}});
        total = trackList.size();
        std::vector<remote::TrackSummary> page;
        for (std::size_t i = offset; i < trackList.size() && page.size() < limit; ++i) {
            page.push_back(trackList[i]);
        }
        return page;
    }

    std::optional<remote::TrackDetail> track(TrackId id) override {
        note(Call{"track", {id}, 0.0, {}});
        return trackDetail;
    }

    std::string addUrls(std::vector<std::string>   urls,
                        std::optional<std::size_t> at) override {
        note(Call{"addUrls", {}, at ? static_cast<double>(*at) : -1.0,
              urls.empty() ? std::string{} : urls.front()});
        return jobId;
    }

    remote::Outcome removeTracks(const std::vector<TrackId>& ids) override {
        note(Call{"removeTracks", ids, 0.0, {}});
        return outcome;
    }
    remote::Outcome moveTracks(const std::vector<TrackId>& ids, TrackId anchor) override {
        note(Call{"moveTracks", ids, static_cast<double>(anchor), {}});
        return outcome;
    }
    remote::Outcome randomize() override { note(Call{"randomize"}); return outcome; }
    remote::Outcome clearPlaylist() override { note(Call{"clearPlaylist"}); return outcome; }

    remote::Outcome setQueued(const std::vector<TrackId>& ids, bool queued) override {
        note(Call{"setQueued", ids, queued ? 1.0 : 0.0, {}});
        return outcome;
    }
    remote::Outcome clearQueue() override { note(Call{"clearQueue"}); return outcome; }
    remote::Outcome setStopAfter(const std::vector<TrackId>& ids, bool stopAfter) override {
        note(Call{"setStopAfter", ids, stopAfter ? 1.0 : 0.0, {}});
        return outcome;
    }
    remote::Outcome resetPlayCount(const std::vector<TrackId>& ids) override {
        note(Call{"resetPlayCount", ids, 0.0, {}});
        return outcome;
    }
    remote::Outcome setRating(const std::vector<TrackId>& ids,
                              std::optional<double>       rating) override {
        note(Call{"setRating", ids, rating.value_or(-1.0), {}});
        return outcome;
    }
    remote::Outcome undo() override { note(Call{"undo"}); return outcome; }
    remote::Outcome redo() override { note(Call{"redo"}); return outcome; }

    std::vector<remote::SettingInfo> settings() override {
        note(Call{"settings"});
        return settingList;
    }
    std::optional<remote::SettingInfo> setting(std::string_view key) override {
        note(Call{"setting", {}, 0.0, std::string{key}});
        return settingValue;
    }
    remote::SettingWrite setSetting(std::string_view key, std::string_view value) override {
        note(Call{"setSetting", {}, 0.0, std::string{key} + "=" + std::string{value}});
        return settingWrite;
    }

    remote::EqualizerState equalizer() override {
        note(Call{"equalizer"});
        return equalizerState;
    }
    remote::Outcome setEqualizer(
        std::optional<bool> enabled, std::optional<double> preamp,
        const std::vector<std::pair<double, double>>& bands) override {
        note(Call{"setEqualizer", {}, preamp.value_or(0.0),
              std::to_string(bands.size()) + (enabled.value_or(false) ? " on" : " off")});
        return outcome;
    }
    std::vector<std::string> equalizerPresets() override {
        note(Call{"equalizerPresets"});
        return presetList;
    }
    remote::Outcome applyEqualizerPreset(std::string_view name) override {
        note(Call{"applyEqualizerPreset", {}, 0.0, std::string{name}});
        return outcome;
    }

    std::shared_ptr<const std::vector<std::byte>> artwork(TrackId id) override {
        note(Call{"artwork", {id}, 0.0, {}});
        return artworkValue;
    }

    std::optional<remote::JobStatus> job(std::string_view id) override {
        note(Call{"job", {}, 0.0, std::string{id}});
        return jobValue;
    }

private:
    void note(Call call) {
        std::lock_guard guard(mutex_);
        calls_.push_back(std::move(call));
    }

    mutable std::mutex mutex_;
    std::vector<Call>  calls_;
};

}  // namespace xpcog::test
