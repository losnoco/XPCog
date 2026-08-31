// The player, as the REST remote control is allowed to see it.
//
// core defines IPlayerControl and cannot implement it: the transport is
// PlaybackController, the edits are AppCommands, and the undo stack, the library
// and the playlist view all live up here with the window. This is the class that
// puts them behind that interface.
//
// **Every method runs on the interface thread.** CallGate puts them there and
// waits, so this may touch Playlist, UndoStack, Library and Settings unlocked,
// exactly as a menu handler does -- and must never be called from anywhere else.
//
// Two things it does that are not obvious from the interface:
//
// It publishes `settingChanged` after a settings write, which makes it the
// fourth publisher of that signal beside PreferencesDialog, EqualizerPanel and
// SpeedPanel. Without it a remote write would be stored and inert until the next
// launch, which is the bug alwaysStopAfterCurrent already was once.
//
// And it reports PlaybackController::busy() as Outcome::Busy. The controller
// silently declines commands while a start is in flight, which is right for a
// menu item and wrong for something that has to say what happened.

#pragma once

#include "AppCommands.hpp"
#include "RemoteJobs.hpp"

#include "xpcog/core/Signal.hpp"
#include "xpcog/core/remote/PlayerControl.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xpcog {
class Library;
class Playlist;
class PlaylistView;
class Settings;
class UndoStack;
}  // namespace xpcog

namespace xpcog::app {

class PlaybackController;

class AppPlayerControl : public remote::IPlayerControl {
public:
    /// What the window has to do that this cannot: start a scan, which needs the
    /// PluginCache and the one-at-a-time queue MainFrame owns.
    using ScanStarter = std::function<std::string(std::vector<std::string> urls,
                                                  std::optional<std::size_t> at)>;

    AppPlayerControl(PlaybackController& playback, Playlist& playlist,
                     PlaylistView& view, AppCommands& commands, Settings& settings,
                     RemoteJobs& jobs, Library* library, ScanStarter startScan);

    /// Published after a settings write, for MainFrame::onSettingChanged.
    ///
    /// The same Signal<std::string> the preferences dialog and the two panels
    /// publish, wired to the same handler -- a fourth publisher rather than a
    /// second mechanism.
    Signal<std::string> settingChanged;

    /// The session id in every Status. Random per launch, because track ids are
    /// not persisted and the same number means a different track after a
    /// restart.
    [[nodiscard]] const std::string& sessionId() const noexcept { return sessionId_; }

    // --- IPlayerControl ----------------------------------------------------

    remote::Status status() override;

    remote::Outcome play(std::optional<TrackId> id) override;
    remote::Outcome pause() override;
    remote::Outcome playPause() override;
    remote::Outcome stop() override;
    remote::Outcome next() override;
    remote::Outcome previous() override;
    remote::Outcome seek(double seconds) override;
    remote::Outcome setVolume(double linear) override;
    remote::Outcome setOrder(std::optional<std::string> repeat,
                             std::optional<std::string> shuffle,
                             std::optional<bool>        stopAfterCurrent) override;

    std::vector<remote::TrackSummary> tracks(std::size_t offset, std::size_t limit,
                                             std::string_view query,
                                             std::size_t&     total) override;
    std::optional<remote::TrackDetail> track(TrackId id) override;

    std::string addUrls(std::vector<std::string>   urls,
                        std::optional<std::size_t> at) override;
    remote::Outcome removeTracks(const std::vector<TrackId>& ids) override;
    remote::Outcome moveTracks(const std::vector<TrackId>& ids, TrackId anchor) override;
    remote::Outcome randomize() override;
    remote::Outcome clearPlaylist() override;
    remote::Outcome setQueued(const std::vector<TrackId>& ids, bool queued) override;
    remote::Outcome clearQueue() override;
    remote::Outcome setStopAfter(const std::vector<TrackId>& ids, bool stopAfter) override;
    remote::Outcome resetPlayCount(const std::vector<TrackId>& ids) override;
    remote::Outcome setRating(const std::vector<TrackId>& ids,
                              std::optional<double> rating) override;
    remote::Outcome undo() override;
    remote::Outcome redo() override;

    std::vector<remote::SettingInfo> settings() override;
    std::optional<remote::SettingInfo> setting(std::string_view key) override;
    remote::SettingWrite setSetting(std::string_view key, std::string_view value) override;

    remote::EqualizerState equalizer() override;
    remote::Outcome setEqualizer(
        std::optional<bool> enabled, std::optional<double> preamp,
        const std::vector<std::pair<double, double>>& bands) override;
    std::vector<std::string> equalizerPresets() override;
    remote::Outcome applyEqualizerPreset(std::string_view name) override;

    std::shared_ptr<const std::vector<std::byte>> artwork(TrackId id) override;
    std::optional<remote::JobStatus> job(std::string_view id) override;

private:
    [[nodiscard]] remote::SettingInfo describe(std::string_view key) const;

    PlaybackController& playback_;
    Playlist&           playlist_;
    PlaylistView&       view_;
    AppCommands&        commands_;
    Settings&           settings_;
    RemoteJobs&         jobs_;
    Library*            library_ = nullptr;
    ScanStarter         startScan_;
    std::string         sessionId_;

    /// Bumped on every playlist change, and published in Status.
    ///
    /// With sessionId it is how a client notices that the ids it cached no
    /// longer mean what they meant -- nothing persists a TrackId, so the same
    /// number is a different track after a restart, and acting on a stale one
    /// deletes the wrong thing.
    std::uint64_t revision_ = 0;
    Subscription  playlistWatch_;
};

}  // namespace xpcog::app
