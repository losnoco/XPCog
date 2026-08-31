// The player as xpcog-cli can offer it: an engine, a playlist, and no window.
//
// The point of this class is not that it is a good headless player -- it is that
// the seam works. core defines IPlayerControl, the application implements it over
// PlaybackController and AppCommands, and this implements the same interface over
// an AudioEngine and a Playlist directly. If a route can be driven from here with
// no toolkit linked at all, the layering the whole project rests on is intact.
//
// A SerialExecutor stands in for the interface thread. Everything the gate
// dispatches is posted to it, so this class is called one at a time from one
// thread, exactly as the application's implementation is -- and Playlist,
// UndoStack and Settings can be touched unlocked here for the same reason.
//
// --- What it cannot do ------------------------------------------------------
//
// Several things, and they answer Outcome::Unsupported -- 501 -- rather than
// pretending. `serve --help` lists them, and so does docs/REST.md:
//
//   * Next and Previous do not skip a track that will not open. The hunt for a
//     playable entry is PlaybackController's, and it is a hundred lines of
//     app-layer bookkeeping; here a bad file simply stops playback.
//   * There is no 409. PlaybackController's starting_/stopping_ guards are what
//     produce it; the executor serialises instead, so a slow URL holds its queue
//     and requests time out at the gate as 503.
//   * No output-device switching under a running stream, no resume-at, no cover
//     art (that is content-addressed in a library this does not open), and no
//     desktop integration of any kind.
//   * Undo labels are English. core has no catalogue and never will.

#pragma once

#include "xpcog/core/SerialExecutor.hpp"
#include "xpcog/core/UndoStack.hpp"
#include "xpcog/core/audio/AudioEngine.hpp"
#include "xpcog/core/library/Playlist.hpp"
#include "xpcog/core/library/PlaylistView.hpp"
#include "xpcog/core/remote/PlayerControl.hpp"

#include <atomic>
#include <string>

namespace xpcog::cli {

class CliPlayerControl : public remote::IPlayerControl, public AudioEngine::Delegate {
public:
    CliPlayerControl(const PluginRegistry& registry, AudioEngine& engine,
                     Settings& settings);

    // --- AudioEngine::Delegate ---------------------------------------------
    //
    // Called on the feeder thread, which is why the two members they touch are
    // atomic and nothing else here is.
    std::optional<Url> nextTrack() override;
    void               trackBegan(const Url& url) override;
    void               stoppedNaturally() override;
    void               trackFailed(const Url& url) override;

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

    /// The playlist, for the command that seeds it before the server starts.
    [[nodiscard]] Playlist& playlist() noexcept { return playlist_; }

private:
    [[nodiscard]] remote::Outcome startTrack(TrackId id);

    const PluginRegistry& registry_;
    AudioEngine&          engine_;
    Settings&             settings_;

    Playlist     playlist_;
    PlaylistView view_{playlist_};
    UndoStack    undo_;

    /// Written from the feeder thread, read from the executor's.
    std::atomic<TrackId> audible_{kInvalidTrackId};

    std::uint64_t revision_ = 0;
    Subscription  playlistWatch_;
    std::string   sessionId_;
};

}  // namespace xpcog::cli
