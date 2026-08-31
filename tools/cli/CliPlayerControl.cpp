#include "CliPlayerControl.hpp"

#include "xpcog/core/PluginRegistry.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/EqualizerPresets.hpp"
#include "xpcog/core/library/PlaylistCommands.hpp"
#include "xpcog/core/library/Scanner.hpp"
#include "xpcog/core/remote/Token.hpp"

#include <algorithm>
#include <filesystem>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace xpcog::cli {
namespace {

using remote::Outcome;

std::string nameOfRepeat(RepeatMode mode) {
    switch (mode) {
        case RepeatMode::None:  return "none";
        case RepeatMode::One:   return "one";
        case RepeatMode::Album: return "album";
        case RepeatMode::All:   return "all";
    }
    return "none";
}

std::string nameOfShuffle(ShuffleMode mode) {
    switch (mode) {
        case ShuffleMode::Off:    return "off";
        case ShuffleMode::Albums: return "albums";
        case ShuffleMode::All:    return "all";
    }
    return "off";
}

remote::TrackSummary summarise(const PlaylistEntry& entry) {
    remote::TrackSummary track;
    track.id        = entry.id;
    track.url       = entry.url.toString();
    track.title     = entry.title();
    track.artist    = entry.artist.str();
    track.album     = entry.album.str();
    track.duration  = entry.duration();
    track.track     = entry.track;
    track.disc      = entry.disc;
    track.playCount = entry.playCount;
    track.queued    = entry.queued();
    track.stopAfter = entry.stopAfter;
    track.error     = entry.error;
    return track;
}

}  // namespace

CliPlayerControl::CliPlayerControl(const PluginRegistry& registry, AudioEngine& engine,
                                   Settings& settings)
    : registry_(registry),
      engine_(engine),
      settings_(settings),
      sessionId_(remote::generateRemoteToken().substr(0, 16)) {
    playlistWatch_ = playlist_.observe([this](const Playlist::Change&) { ++revision_; });
}

// --- AudioEngine::Delegate --------------------------------------------------

std::optional<Url> CliPlayerControl::nextTrack() {
    // Called on the feeder thread, so it must not touch the playlist -- which is
    // the executor's. Gapless advance is therefore not offered here: the engine
    // stops at the end of a track and a client asks for the next one. The
    // application's implementation has PlaybackController for this, and that is
    // the difference this class exists to make visible.
    return std::nullopt;
}

void CliPlayerControl::trackBegan(const Url& /*url*/) {}

void CliPlayerControl::stoppedNaturally() {
    audible_.store(kInvalidTrackId, std::memory_order_release);
}

void CliPlayerControl::trackFailed(const Url& /*url*/) {
    audible_.store(kInvalidTrackId, std::memory_order_release);
}

// --- IPlayerControl ---------------------------------------------------------

remote::Status CliPlayerControl::status() {
    remote::Status status;
    const PlaybackStatus playing = engine_.status();
    status.playing      = playing != PlaybackStatus::Stopped;
    status.paused       = playing == PlaybackStatus::Paused;
    status.currentTrack = audible_.load(std::memory_order_acquire);
    status.position     = engine_.trackPositionSeconds();
    status.volume       = engine_.volume();

    if (const PlaylistEntry* entry = playlist_.find(status.currentTrack);
        entry != nullptr) {
        status.duration = entry->duration();
    }

    status.repeat           = nameOfRepeat(playlist_.repeat());
    status.shuffle          = nameOfShuffle(playlist_.shuffle());
    status.stopAfterCurrent = playlist_.stopAfterCurrent();
    status.playlistSize     = playlist_.size();
    status.playlistRevision = revision_;
    status.sessionId        = sessionId_;
    return status;
}

remote::Outcome CliPlayerControl::startTrack(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        return Outcome::NotFound;
    }
    const Url url = entry->url;

    // Blocking, on the executor's thread, which is what makes a slow URL hold
    // the queue and every request behind it time out at the gate. That is the
    // honest failure for this host: there are no starting_/stopping_ guards to
    // answer 409 with.
    engine_.stop();
    if (!engine_.play(url)) {
        audible_.store(kInvalidTrackId, std::memory_order_release);
        return Outcome::Rejected;
    }
    audible_.store(id, std::memory_order_release);
    playlist_.setCurrent(id);
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::play(std::optional<TrackId> id) {
    if (id) {
        return startTrack(*id);
    }
    if (engine_.status() == PlaybackStatus::Paused) {
        engine_.resume();
        return Outcome::Ok;
    }
    if (engine_.status() == PlaybackStatus::Playing) {
        return Outcome::Ok;
    }
    if (const std::optional<TrackId> current = playlist_.current()) {
        return startTrack(*current);
    }
    if (playlist_.size() == 0) {
        return Outcome::NotFound;
    }
    return startTrack(playlist_.at(0).id);
}

remote::Outcome CliPlayerControl::pause() {
    if (engine_.status() == PlaybackStatus::Playing) {
        engine_.pause();
    }
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::playPause() {
    switch (engine_.status()) {
        case PlaybackStatus::Playing: engine_.pause(); return Outcome::Ok;
        case PlaybackStatus::Paused:  engine_.resume(); return Outcome::Ok;
        case PlaybackStatus::Stopped: return play(std::nullopt);
    }
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::stop() {
    engine_.stop();
    audible_.store(kInvalidTrackId, std::memory_order_release);
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::next() {
    // No hunt for a playable entry: that is PlaybackController's, and it is
    // app-layer. A track that will not open stops playback here rather than
    // being stepped over.
    if (!playlist_.next()) {
        return Outcome::NotFound;
    }
    const std::optional<TrackId> current = playlist_.current();
    return current ? startTrack(*current) : Outcome::NotFound;
}

remote::Outcome CliPlayerControl::previous() {
    if (!playlist_.previous()) {
        return Outcome::NotFound;
    }
    const std::optional<TrackId> current = playlist_.current();
    return current ? startTrack(*current) : Outcome::NotFound;
}

remote::Outcome CliPlayerControl::seek(double seconds) {
    if (engine_.status() == PlaybackStatus::Stopped) {
        return Outcome::Rejected;
    }
    return engine_.seek(seconds) ? Outcome::Ok : Outcome::Rejected;
}

remote::Outcome CliPlayerControl::setVolume(double linear) {
    engine_.setVolume(static_cast<float>(linear));
    settings_.setRawValue("volume", std::to_string(linear));
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::setOrder(std::optional<std::string> repeat,
                                           std::optional<std::string> shuffle,
                                           std::optional<bool> stopAfterCurrent) {
    if (repeat) {
        if (*repeat == "none")       { playlist_.setRepeat(RepeatMode::None); }
        else if (*repeat == "one")   { playlist_.setRepeat(RepeatMode::One); }
        else if (*repeat == "album") { playlist_.setRepeat(RepeatMode::Album); }
        else if (*repeat == "all")   { playlist_.setRepeat(RepeatMode::All); }
        else { return Outcome::Rejected; }
    }
    if (shuffle) {
        if (*shuffle == "off")         { playlist_.setShuffle(ShuffleMode::Off); }
        else if (*shuffle == "albums") { playlist_.setShuffle(ShuffleMode::Albums); }
        else if (*shuffle == "all")    { playlist_.setShuffle(ShuffleMode::All); }
        else { return Outcome::Rejected; }
    }
    if (stopAfterCurrent) {
        playlist_.setStopAfterCurrent(*stopAfterCurrent);
    }
    return Outcome::Ok;
}

std::vector<remote::TrackSummary> CliPlayerControl::tracks(std::size_t offset,
                                                           std::size_t limit,
                                                           std::string_view query,
                                                           std::size_t& total) {
    std::vector<remote::TrackSummary> page;
    total = 0;
    for (std::size_t row = 0; row < view_.rowCount(); ++row) {
        const PlaylistEntry* entry = view_.entryAt(row);
        if (entry == nullptr || !playlistEntryMatches(*entry, query)) {
            continue;
        }
        const std::size_t index = total++;
        if (index >= offset && page.size() < limit) {
            page.push_back(summarise(*entry));
        }
    }
    return page;
}

std::optional<remote::TrackDetail> CliPlayerControl::track(TrackId id) {
    const PlaylistEntry* entry = playlist_.find(id);
    if (entry == nullptr) {
        return std::nullopt;
    }
    remote::TrackDetail detail;
    detail.summary      = summarise(*entry);
    detail.errorMessage = entry->errorMessage;
    detail.genre        = entry->genre.str();
    detail.composer     = entry->composer.str();
    detail.date         = entry->date.str();
    detail.comment      = entry->comment.str();
    detail.albumArtist  = entry->albumArtist.str();

    const TrackProperties& properties = entry->properties;
    detail.sampleRate    = properties.format.sampleRate;
    detail.channels      = static_cast<int>(properties.format.channels);
    detail.bitsPerSample = static_cast<int>(properties.format.bitsPerSample);
    detail.bitrateKbps   = properties.bitrateKbps;
    detail.seekable      = properties.seekable;
    detail.lossless      = properties.lossless;
    detail.cuesheet      = properties.cuesheet.has_value();
    return detail;
}

std::string CliPlayerControl::addUrls(std::vector<std::string>   urls,
                                      std::optional<std::size_t> at) {
    // Scanned inline rather than as a job. This host has no queue to contend
    // with -- the executor is the queue -- so the only cost is that a very large
    // folder times out at the gate, which the 503 says plainly.
    std::vector<Url> inputs;
    for (const std::string& text : urls) {
        if (std::optional<Url> url = Url::parse(text); url && !url->scheme().empty()) {
            inputs.push_back(*std::move(url));
        } else {
            inputs.push_back(Url::fromLocalPath(std::filesystem::path{text}));
        }
    }
    if (inputs.empty()) {
        return {};
    }

    Scanner                    scanner(registry_);
    std::vector<PlaylistEntry> found = scanner.scan(inputs);
    if (found.empty()) {
        return {};
    }

    const std::size_t where = at.value_or(playlist_.size());
    undo_.push(std::make_unique<InsertTracksCommand>(
        playlist_, where, std::move(found), "Add tracks"));

    // A synchronous scan still has to answer with a job id, because the route
    // promises one. It is already finished by the time the client reads it.
    return "inline";
}

remote::Outcome CliPlayerControl::removeTracks(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        return Outcome::NotFound;
    }
    undo_.push(std::make_unique<RemoveTracksCommand>(
        playlist_, ids, "Remove tracks"));
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::moveTracks(const std::vector<TrackId>& ids,
                                             TrackId                     anchor) {
    std::vector<TrackId> after = orderAfterMove(playlist_, ids, anchor);
    if (after == currentOrder(playlist_)) {
        return Outcome::Ok;
    }
    undo_.push(std::make_unique<ReorderCommand>(playlist_, std::move(after),
                                                "Move tracks"));
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::randomize() {
    if (playlist_.size() <= 1) {
        return Outcome::Ok;
    }
    undo_.push(std::make_unique<RandomizeCommand>(playlist_, "Randomize"));
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::clearPlaylist() {
    std::vector<TrackId> ids = currentOrder(playlist_);
    if (ids.empty()) {
        return Outcome::Ok;
    }
    undo_.push(std::make_unique<RemoveTracksCommand>(playlist_, std::move(ids),
                                                     "Clear playlist"));
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::setQueued(const std::vector<TrackId>& ids,
                                            bool                        queued) {
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        if (queued) {
            playlist_.enqueue(id);
        } else {
            playlist_.dequeue(id);
        }
    }
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::clearQueue() {
    playlist_.clearQueue();
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::setStopAfter(const std::vector<TrackId>& ids,
                                               bool                        stopAfter) {
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        playlist_.update(id,
                         [stopAfter](PlaylistEntry& entry) { entry.stopAfter = stopAfter; });
    }
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::resetPlayCount(const std::vector<TrackId>& ids) {
    for (const TrackId id : ids) {
        if (playlist_.find(id) == nullptr) {
            continue;
        }
        playlist_.update(id, [](PlaylistEntry& entry) { entry.playCount = 0; });
    }
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::setRating(const std::vector<TrackId>& /*ids*/,
                                            std::optional<double> /*rating*/) {
    // Ratings live in the SQLite library, which this host does not open.
    return Outcome::Unsupported;
}

remote::Outcome CliPlayerControl::undo() {
    undo_.undo();
    return Outcome::Ok;
}

remote::Outcome CliPlayerControl::redo() {
    undo_.redo();
    return Outcome::Ok;
}

std::vector<remote::SettingInfo> CliPlayerControl::settings() {
    std::vector<remote::SettingInfo> all;
    for (const Settings::Desc& desc : Settings::all()) {
        all.push_back(*setting(desc.key));
    }
    return all;
}

std::optional<remote::SettingInfo> CliPlayerControl::setting(std::string_view key) {
    for (const Settings::Desc& desc : Settings::all()) {
        if (desc.key != key) {
            continue;
        }
        remote::SettingInfo info;
        info.key          = std::string{key};
        info.type         = std::string{desc.type};
        info.value        = settings_.rawValue(key);
        info.defaultValue = Settings::defaultValue(key);
        info.writable     = true;
        // This host reloads the DSP and nothing else, so it says so rather than
        // repeating the application's much larger table of what takes effect
        // when. Answering "immediately" for everything would be the easy lie.
        info.appliesFrom = key.starts_with("eq") || key.starts_with("rubberband") ||
                                   key == "GraphicEQenable" || key == "eqPreamp" ||
                                   key == "pitch" || key == "tempo"
                               ? "immediately"
                               : "nextTrack";
        return info;
    }
    return std::nullopt;
}

remote::SettingWrite CliPlayerControl::setSetting(std::string_view key,
                                                  std::string_view value) {
    const std::optional<remote::SettingInfo> existing = setting(key);
    if (!existing) {
        return {Outcome::NotFound, {}, "No setting with that key."};
    }
    settings_.setRawValue(key, value);
    // The only fan-out this host has.
    engine_.reloadDsp();
    return {Outcome::Ok, existing->appliesFrom, {}};
}

remote::EqualizerState CliPlayerControl::equalizer() {
    remote::EqualizerState state;
    state.enabled    = settings_.GraphicEqEnable();
    state.preamp     = settings_.EqPreamp();
    state.trackGenre = settings_.GraphicEqTrackGenre();
    state.preset     = equalizerPresetName(settings_);

    const std::span<const double>      frequencies = Equalizer::bandFrequencies();
    const std::span<const char* const> keys        = Equalizer::bandSettingsKeys();
    for (std::size_t i = 0; i < frequencies.size() && i < keys.size(); ++i) {
        double            gain = 0.0;
        const std::string raw  = settings_.rawValue(keys[i]);
        try {
            gain = raw.empty() ? 0.0 : std::stod(raw);
        } catch (const std::exception&) {
            gain = 0.0;
        }
        state.bands.emplace_back(frequencies[i], gain);
    }
    return state;
}

remote::Outcome CliPlayerControl::setEqualizer(
    std::optional<bool> enabled, std::optional<double> preamp,
    const std::vector<std::pair<double, double>>& bands) {
    const std::span<const double>      frequencies = Equalizer::bandFrequencies();
    const std::span<const char* const> keys        = Equalizer::bandSettingsKeys();

    std::vector<std::pair<std::size_t, double>> resolved;
    for (const auto& [hz, gain] : bands) {
        const auto found = std::find(frequencies.begin(), frequencies.end(), hz);
        if (found == frequencies.end()) {
            return Outcome::Rejected;
        }
        resolved.emplace_back(
            static_cast<std::size_t>(std::distance(frequencies.begin(), found)), gain);
    }

    if (enabled) {
        settings_.setRawValue("GraphicEQenable", *enabled ? "true" : "false");
    }
    if (preamp) {
        settings_.setRawValue("eqPreamp", std::to_string(*preamp));
    }
    for (const auto& [index, gain] : resolved) {
        settings_.setRawValue(keys[index], std::to_string(gain));
    }
    if (!resolved.empty() || preamp) {
        markEqualizerCustom(settings_);
    }
    engine_.reloadDsp();
    return Outcome::Ok;
}

std::vector<std::string> CliPlayerControl::equalizerPresets() {
    std::vector<std::string> names;
    for (const EqualizerPreset& preset : shippedEqualizerPresets().presets()) {
        names.push_back(preset.name);
    }
    return names;
}

remote::Outcome CliPlayerControl::applyEqualizerPreset(std::string_view name) {
    if (!applyEqualizerPresetByName(settings_, name)) {
        return Outcome::NotFound;
    }
    engine_.reloadDsp();
    return Outcome::Ok;
}

std::shared_ptr<const std::vector<std::byte>> CliPlayerControl::artwork(TrackId /*id*/) {
    // Content-addressed in the SQLite library, which this host does not open.
    return nullptr;
}

std::optional<remote::JobStatus> CliPlayerControl::job(std::string_view id) {
    if (id != "inline") {
        return std::nullopt;
    }
    // Scans here are synchronous, so the only job that can be asked about has
    // already finished.
    remote::JobStatus job;
    job.id    = "inline";
    job.state = "done";
    return job;
}

}  // namespace xpcog::cli
